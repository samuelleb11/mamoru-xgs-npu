// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * mamoru-agnic P3: the GIU management command channel.
 *
 * Publishes the host CMD (host->dev) and NOTIF (dev->host) descriptor rings into the
 * GIU config_mem, asserts HOST_MGMT_READY, and rendezvouses with the NPU's nmp
 * (in dp_fwd) which answers DEV_MGMT_READY. A continuous notif-ring drainer then
 * carries command responses back to the single armed waiter and consumes async
 * keep-alives / link-change notifications.
 *
 * KEY ABI (transcribed from the BSD-2 FreeBSD agnic_mgmt.c): the ring producer/
 * consumer INDEX words live on BAR0, in the array the device places at
 * config_mem.dev_use_size; only the 64-byte descriptor ring BODY is host DRAM the
 * device DMAs (36-bit). The mgmt path is poll-driven (the device polls the producer
 * index); we also ring the FLR-safe h2t BAR4 doorbell as a courtesy kick.
 *
 * No NPU reset/FLR anywhere.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/unaligned.h>
#include "agnic.h"

#define MGMT_H2T_DBELL_MS	2000
#define MGMT_CMD_MS		5000
#define MGMT_DRAIN_POLL_MS	2
#define MGMT_DRAIN_DEAD_MS	1000	/* back-off poll while the EP is unresponsive */

/* A PCIe read that gets an Unsupported Request / no completion reads back as all-ones.
 * Treat that on the notif producer index as "the endpoint is gone" rather than a valid
 * index, so the drainer can back off instead of hammering a dead bus every 2 ms. */
#define AGNIC_MMIO_DEAD		0xffffffffU

/* Poll a BAR0 dword until (val & mask) == expect, or timeout. 0 on match. */
static int agnic_poll(struct agnic *ag, int bar, u32 off, u32 mask, u32 expect, int timeout_ms)
{
	int spent;

	for (spent = 0; spent <= timeout_ms; spent += 2) {
		if ((agnic_rd(ag, bar, off) & mask) == expect)
			return 0;
		usleep_range(1500, 2500);
	}
	return -ETIMEDOUT;
}

/* Write one agnic_q_hw_info block (24B) into config_mem at BAR0 offset q_off. */
static void mgmt_publish_q(struct agnic *ag, u32 q_off, const struct agnic_mgmt_ring *r)
{
	u64 addr = r->mem.da;

	agnic_wr(ag, AGNIC_BAR_GIU, q_off + AGNIC_QHW_ADDR_OFF, lower_32_bits(addr));
	agnic_wr(ag, AGNIC_BAR_GIU, q_off + AGNIC_QHW_ADDR_OFF + 4, upper_32_bits(addr));
	agnic_wr(ag, AGNIC_BAR_GIU, q_off + AGNIC_QHW_PROD_OFF, r->pub_prod_off);
	agnic_wr(ag, AGNIC_BAR_GIU, q_off + AGNIC_QHW_CONS_OFF, r->pub_cons_off);
	agnic_wr(ag, AGNIC_BAR_GIU, q_off + AGNIC_QHW_LEN_OFF, r->count);
}

/* Latch the h2t mgmt doorbell (ctrl_map.h2t_dbell_msg[0]) — best-effort BAR4 kick. */
static void mgmt_latch_dbell(struct agnic *ag)
{
	int cbar = ag->fac_bar[AGNIC_FAC_CONTROL];
	u32 coff = ag->fac_off[AGNIC_FAC_CONTROL];
	u32 base, cnt, alo, ahi;
	u64 addr;
	int spent;

	ag->h2t_valid = false;
	if (cbar < 0)
		return;
	if (agnic_poll(ag, cbar, coff + AGNIC_CTRL_HANDSHAKE_OFF,
		       CTRL_FCLT_TRGT_H2T_DBELL, CTRL_FCLT_TRGT_H2T_DBELL, MGMT_H2T_DBELL_MS)) {
		dev_info(ag->dev, "P3: no h2t doorbell (device polls the producer index)\n");
		return;
	}
	cnt = agnic_rd(ag, cbar, coff + AGNIC_CTRL_H2T_DBELL_CNT_OFF);
	if (cnt <= AGNIC_H2T_DBELL_MGMT)
		return;
	base = coff + AGNIC_CTRL_H2T_DBELL_MSG(AGNIC_H2T_DBELL_MGMT);
	for (spent = 0; spent <= MGMT_H2T_DBELL_MS; spent += 2) {
		alo = agnic_rd(ag, cbar, base + AGNIC_DBELL_MSG_ADDR_OFF);
		ahi = agnic_rd(ag, cbar, base + AGNIC_DBELL_MSG_ADDR_OFF + 4);
		addr = ((u64)ahi << 32) | alo;
		if (addr)
			break;
		usleep_range(1500, 2500);
	}
	if (!addr)
		return;
	ag->h2t_bar4_off = (u32)(addr & 0xffffffU);	/* BAR4-relative reg offset */
	ag->h2t_data = agnic_rd(ag, cbar, base + AGNIC_DBELL_MSG_DATA_OFF);
	ag->h2t_valid = true;
	dev_info(ag->dev, "P3: h2t mgmt doorbell latched: BAR4+0x%06x data 0x%08x\n",
		 ag->h2t_bar4_off, ag->h2t_data);
}

/* cmd_idx cookie: rolls 1..1023 (never 0 or 0xFFFF). Caller holds drain_lock. */
static u16 next_cmd_idx(struct agnic *ag)
{
	u16 v = ag->cmd_idx_gen + 1;

	if (v >= AGNIC_CMD_COOKIE_COUNT)
		v = 1;
	ag->cmd_idx_gen = v;
	return v;
}

/* Fill one cmd descriptor and advance the producer shadow. Caller holds drain_lock and
 * PUBLISHES the index itself once the lock is dropped — see mgmt_drain() for why no MMIO
 * may happen under drain_lock. Returns the producer value the caller must write. */
static u32 mgmt_post(struct agnic *ag, u16 idx, u8 cmd_code, u8 flags,
		     const void *params, size_t plen)
{
	struct agnic_mgmt_ring *r = &ag->cmd_ring;
	struct agnic_cmd_desc *d = (struct agnic_cmd_desc *)((u8 *)r->mem.va +
			(size_t)r->prod_shadow * AGNIC_MGMT_DESC_SIZE);

	memset(d, 0, sizeof(*d));
	d->cmd_idx = cpu_to_le16(idx);
	d->app_code = cpu_to_le16(AGNIC_AC_PF_MANAGER);
	d->cmd_code = cmd_code;
	d->client_type = AGNIC_CDT_PF;
	d->flags = flags;
	if (params && plen)
		memcpy(d->data, params, min(plen, (size_t)AGNIC_MGMT_DESC_DATA_LEN));

	dma_wmb();	/* descriptor visible in host DRAM before the producer index */
	r->prod_shadow = AGNIC_RING_INC(r->prod_shadow, r->count);
	return r->prod_shadow;
}

/*
 * Drain the notif ring: dispatch a response to the armed waiter, consume async
 * notifications, advance the consumer index.
 *
 * MMIO IS DELIBERATELY OUTSIDE drain_lock. ioread32()/iowrite32() to this endpoint can
 * stall for the PCIe completion timeout whenever the NPU is unresponsive (rebooting, GIU
 * wedged, EP in reset). Doing that with the lock held and IRQs disabled soft-locks every
 * CPU that wants the lock: observed on real hardware 2026-08-08, console flooded with
 * `_raw_spin_lock_irqsave <- drain_work_fn` traces and only a mains cycle recovered it.
 * The lock now covers pure memory work — shadow indices and waiter state — and nothing else.
 *
 * Returns false if the endpoint has stopped answering, so the caller can stop re-arming.
 */
static bool mgmt_drain(struct agnic *ag)
{
	struct agnic_mgmt_ring *r = &ag->notif_ring;
	unsigned long flags;
	u32 prod, cons_pub;
	u32 scanned = 0;
	bool advanced = false;

	prod = agnic_rd(ag, AGNIC_BAR_GIU, r->prod_bar_off);
	if (prod == AGNIC_MMIO_DEAD)		/* PCIe UR completions read back all-ones */
		return false;
	if (prod >= r->count)			/* garbage index: skip this round, stay armed */
		return true;

	dma_rmb();
	spin_lock_irqsave(&ag->drain_lock, flags);
	while (r->cons_shadow != prod && scanned < r->count) {
		struct agnic_cmd_desc *d = (struct agnic_cmd_desc *)((u8 *)r->mem.va +
				(size_t)r->cons_shadow * AGNIC_MGMT_DESC_SIZE);
		u16 rcmd = le16_to_cpu(d->cmd_idx);

		scanned++;
		if (rcmd == AGNIC_CMD_ID_NOTIFICATION) {
			if (d->cmd_code == AGNIC_NC_PF_LINK_CHANGE)
				dev_info(ag->dev, "P3: NC_PF_LINK_CHANGE (status 0x%x)\n",
					 get_unaligned_le32(&d->data[0]));
			/* keep-alive / unknown: consumed, no ack. */
		} else if (ag->wait_idx && rcmd == ag->wait_idx) {
			ag->wait_status = d->data[0];
			if (ag->wait_buf && ag->wait_len)
				memcpy(ag->wait_buf, d->data,
				       min(ag->wait_len, (size_t)AGNIC_MGMT_DESC_DATA_LEN));
			ag->wait_idx = 0;	/* disarm */
			complete(&ag->wait_cmpl);
		}
		r->cons_shadow = AGNIC_RING_INC(r->cons_shadow, r->count);
		advanced = true;
	}
	cons_pub = r->cons_shadow;
	spin_unlock_irqrestore(&ag->drain_lock, flags);

	/* One publish per drain rather than one per descriptor, and outside the lock.
	 * Only this function advances notif cons_shadow, and delayed work never runs
	 * concurrently with itself, so no other writer can race the index backwards. */
	if (advanced)
		agnic_wr(ag, AGNIC_BAR_GIU, r->cons_bar_off, cons_pub);
	return true;
}

static void drain_work_fn(struct work_struct *w)
{
	struct agnic *ag = container_of(to_delayed_work(w), struct agnic, drain_work);

	bool alive = mgmt_drain(ag);
	unsigned int next_ms = MGMT_DRAIN_POLL_MS;

	if (!alive) {
		/* Back off hard rather than hammering a dead bus every 2 ms. We keep polling
		 * (not stop) so the channel self-heals if the NPU comes back: MMIO no longer
		 * runs under drain_lock, so a stalled access blocks only this worker thread
		 * instead of soft-locking every CPU that wants the lock. */
		if (!ag->mgmt_dead) {
			ag->mgmt_dead = true;
			dev_warn(ag->dev,
				 "P3: endpoint stopped answering (BAR reads all-ones); backing off to %u ms\n",
				 MGMT_DRAIN_DEAD_MS);
		}
		next_ms = MGMT_DRAIN_DEAD_MS;
	} else if (ag->mgmt_dead) {
		ag->mgmt_dead = false;
		dev_info(ag->dev, "P3: endpoint answering again — drainer back to %u ms\n",
			 MGMT_DRAIN_POLL_MS);
	}

	if (!ag->mgmt_inited)
		return;			/* teardown in progress: do not re-arm */
	schedule_delayed_work(&ag->drain_work, max(1UL, msecs_to_jiffies(next_ms)));
}

/* One command round-trip: send + wait for the matching response. */
int agnic_mgmt_cmd(struct agnic *ag, u8 cmd_code, const void *params, size_t plen,
		   u8 *resp, size_t resp_len, u8 *out_status)
{
	unsigned long flags;
	long left;
	u32 prod_pub;
	u16 idx;

	if (!ag->mgmt_inited)
		return -ENXIO;
	if (ag->mgmt_dead)
		return -ENODEV;		/* EP gone: fail fast instead of stalling on MMIO */

	mutex_lock(&ag->cmd_lock);
	spin_lock_irqsave(&ag->drain_lock, flags);
	idx = next_cmd_idx(ag);
	ag->wait_idx = idx;
	ag->wait_status = 0xff;
	ag->wait_buf = resp;
	ag->wait_len = resp_len;
	reinit_completion(&ag->wait_cmpl);
	prod_pub = mgmt_post(ag, idx, cmd_code, AGNIC_DESC_FLAGS_SINGLE_RESP, params, plen);
	spin_unlock_irqrestore(&ag->drain_lock, flags);

	/* Publish + kick OUTSIDE drain_lock (see mgmt_drain()). cmd_lock still serialises
	 * posters, so the producer index cannot be written out of order. */
	agnic_wr(ag, AGNIC_BAR_GIU, ag->cmd_ring.prod_bar_off, prod_pub);
	if (ag->h2t_valid)
		agnic_wr(ag, AGNIC_BAR_DBELL, ag->h2t_bar4_off, ag->h2t_data);

	left = wait_for_completion_timeout(&ag->wait_cmpl, msecs_to_jiffies(MGMT_CMD_MS));

	spin_lock_irqsave(&ag->drain_lock, flags);
	if (out_status)
		*out_status = left ? ag->wait_status : 0xff;
	ag->wait_idx = 0;
	ag->wait_buf = NULL;
	spin_unlock_irqrestore(&ag->drain_lock, flags);
	mutex_unlock(&ag->cmd_lock);

	if (!left) {
		dev_warn(ag->dev, "P3: timeout waiting for cmd 0x%02x (idx 0x%04x)\n", cmd_code, idx);
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * P3 (persistent half): allocate + publish the CMD/NOTIF rings into config_mem and
 * assert HOST_MGMT_READY. Once this returns 0 the ring physaddrs are latched by the
 * NPU whenever dp_fwd/nmp starts, and can never be un-published (no FLR) — so post-
 * publish there is no failure path that frees them. The handshake is completed later,
 * retriably, by agnic_mgmt_finish(). Only PRE-publish failures (bad dev_use_size, ring
 * alloc) return an error here, and those are safe.
 */
int agnic_mgmt_publish(struct agnic *ag)
{
	u32 giu_off = ag->fac_off[AGNIC_FAC_GIU];
	u32 idx_base, arr_bytes, st;
	int ret;

	if (ag->fac_bar[AGNIC_FAC_GIU] != AGNIC_BAR_GIU) {
		dev_warn(ag->dev, "P3: GIU config_mem not on BAR0; skip mgmt\n");
		return -EINVAL;
	}

	mutex_init(&ag->cmd_lock);
	spin_lock_init(&ag->drain_lock);
	init_completion(&ag->wait_cmpl);
	INIT_DELAYED_WORK(&ag->drain_work, drain_work_fn);

	/* Where the device placed the ring index array on BAR0. */
	ag->dev_use_size = agnic_rd(ag, AGNIC_BAR_GIU, giu_off + AGNIC_GIU_DEV_USE_SIZE_OFF);
	arr_bytes = AGNIC_MGMT_IDX_SLOTS * sizeof(u32);
	if (!ag->dev_use_size || ag->dev_use_size + arr_bytes > AGNIC_CONFIG_BAR_SIZE) {
		dev_err(ag->dev, "P3: bad dev_use_size 0x%x\n", ag->dev_use_size);
		return -ERANGE;
	}

	ret = agnic_dma_alloc(ag, &ag->cmd_ring.mem, (size_t)AGNIC_CMD_Q_LEN * AGNIC_MGMT_DESC_SIZE);
	if (ret)
		return ret;
	ret = agnic_dma_alloc(ag, &ag->notif_ring.mem, (size_t)AGNIC_NOTIF_Q_LEN * AGNIC_MGMT_DESC_SIZE);
	if (ret)
		goto err_cmd;

	ag->cmd_ring.count = AGNIC_CMD_Q_LEN;
	ag->notif_ring.count = AGNIC_NOTIF_Q_LEN;
	ag->cmd_ring.prod_shadow = ag->cmd_ring.cons_shadow = 0;
	ag->notif_ring.prod_shadow = ag->notif_ring.cons_shadow = 0;

	idx_base = giu_off + ag->dev_use_size;
	ag->cmd_ring.prod_bar_off = idx_base + AGNIC_MGMT_SLOT_CMD_PROD * sizeof(u32);
	ag->cmd_ring.cons_bar_off = idx_base + AGNIC_MGMT_SLOT_CMD_CONS * sizeof(u32);
	ag->notif_ring.prod_bar_off = idx_base + AGNIC_MGMT_SLOT_NOTIF_PROD * sizeof(u32);
	ag->notif_ring.cons_bar_off = idx_base + AGNIC_MGMT_SLOT_NOTIF_CONS * sizeof(u32);
	/* Published offsets are config_mem-base-relative (== dev_use_size + slot*4). */
	ag->cmd_ring.pub_prod_off = ag->dev_use_size + AGNIC_MGMT_SLOT_CMD_PROD * sizeof(u32);
	ag->cmd_ring.pub_cons_off = ag->dev_use_size + AGNIC_MGMT_SLOT_CMD_CONS * sizeof(u32);
	ag->notif_ring.pub_prod_off = ag->dev_use_size + AGNIC_MGMT_SLOT_NOTIF_PROD * sizeof(u32);
	ag->notif_ring.pub_cons_off = ag->dev_use_size + AGNIC_MGMT_SLOT_NOTIF_CONS * sizeof(u32);

	/* Zero our index words, then publish both queues. */
	agnic_wr(ag, AGNIC_BAR_GIU, ag->cmd_ring.prod_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->cmd_ring.cons_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->notif_ring.prod_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->notif_ring.cons_bar_off, 0);
	mgmt_publish_q(ag, giu_off + AGNIC_GIU_CMD_Q_OFF, &ag->cmd_ring);
	mgmt_publish_q(ag, giu_off + AGNIC_GIU_NOTIF_Q_OFF, &ag->notif_ring);
	dev_info(ag->dev, "P3: mgmt rings published (cmd 0x%llx, notif 0x%llx, idx@BAR0+0x%x)\n",
		 (u64)ag->cmd_ring.mem.da, (u64)ag->notif_ring.mem.da, idx_base);

	/* Assert HOST_MGMT_READY (RMW; preserve DEV_READY). Persistent — dp_fwd will see
	 * it and answer DEV_MGMT_READY whenever it starts, so the finish step can retry. */
	st = agnic_rd(ag, AGNIC_BAR_GIU, giu_off + AGNIC_GIU_STATUS_OFF);
	agnic_wr(ag, AGNIC_BAR_GIU, giu_off + AGNIC_GIU_STATUS_OFF,
		 st | AGNIC_CFG_STATUS_HOST_MGMT_READY);
	dev_info(ag->dev, "P3: HOST_MGMT_READY set; awaiting DEV_MGMT_READY (needs dp_fwd/nmp on the NPU)\n");
	return 0;

	/* Only pre-publish allocation failures reach here (notif-ring alloc failed
	 * before mgmt_publish_q); the cmd ring is not yet owned by the device, so it
	 * is safe to free. Post-publish there is no failure path. */
err_cmd:
	agnic_dma_free(ag, &ag->cmd_ring.mem);
	return ret;
}

/*
 * P3 (retriable half): complete the handshake once the NPU answers DEV_MGMT_READY.
 * Returns 0 when the command path is live (mgmt_ready set), or -EAGAIN if the NPU
 * isn't ready yet — either DEV_MGMT_READY not set (dp_fwd/nmp not up), or the post-
 * READY ECHO still failing after a bounded in-place retry. Both cases mean "retry
 * later": HOST_MGMT_READY is already asserted, so this is boot-order agnostic, and a
 * transient NPU stall must not permanently lose the front panel. Never frees the
 * published rings (teardown owns that); idempotent — safe to call until it returns 0.
 */
int agnic_mgmt_finish(struct agnic *ag)
{
	u32 giu_off = ag->fac_off[AGNIC_FAC_GIU];
	u8 resp[AGNIC_MGMT_DESC_DATA_LEN];
	u8 status;
	u32 st;
	int attempt, ret;

	st = agnic_rd(ag, AGNIC_BAR_GIU, giu_off + AGNIC_GIU_STATUS_OFF);
	if (!(st & AGNIC_CFG_STATUS_DEV_MGMT_READY))
		return -EAGAIN;		/* NPU dp_fwd not up yet — caller retries */
	dev_info(ag->dev, "P3: DEV_MGMT_READY — mgmt rings live\n");

	/* Start the continuous drainer BEFORE any command, then latch the doorbell.
	 * finish() is idempotent and retried by the bring-up worker, so clear mgmt_dead:
	 * DEV_MGMT_READY just read back true, meaning the endpoint is answering again. */
	ag->mgmt_dead = false;
	ag->mgmt_inited = true;
	schedule_delayed_work(&ag->drain_work, max(1UL, msecs_to_jiffies(MGMT_DRAIN_POLL_MS)));
	mgmt_latch_dbell(ag);

	/* CC_PF_MGMT_ECHO, bounded in-place retry — ECHO is the FIRST command issued the
	 * instant DEV_MGMT_READY flips, when the NPU nmp cmd loop is least settled, so a
	 * single transient miss must NOT permanently lose the load-bearing front panel
	 * (same reason CC_PF_ENABLE retries in agnic_txrx.c). The drainer stays running
	 * across attempts to carry the response back. */
	for (attempt = 0; attempt < AGNIC_ENABLE_RETRIES; attempt++) {
		status = 0xff;
		ret = agnic_mgmt_cmd(ag, AGNIC_CC_PF_MGMT_ECHO, NULL, 0, resp, sizeof(resp), &status);
		if (!ret && status == AGNIC_NOTIF_STATUS_OK)
			break;
		if (attempt == 0)
			dev_info(ag->dev, "P3: ECHO not ready (err %d status 0x%02x); retrying up to %d\n",
				 ret, status, AGNIC_ENABLE_RETRIES);
		msleep(500);
	}
	if (ret || status != AGNIC_NOTIF_STATUS_OK) {
		dev_warn(ag->dev, "P3: ECHO still failing after %d tries; slow-polling (no FLR)\n",
			 AGNIC_ENABLE_RETRIES);
		/* DEV_MGMT_READY is set and the NPU polls the cmd ring + DMAs the notif ring,
		 * so do NOT free the rings — teardown releases them at detach. Stop our drainer
		 * and return -EAGAIN so the bring-up worker keeps retrying finish() (idempotent):
		 * a transient NPU stall must not permanently lose the front panel. */
		ag->mgmt_inited = false;
		cancel_delayed_work_sync(&ag->drain_work);
		return -EAGAIN;
	}
	ag->mgmt_ready = true;
	dev_info(ag->dev, "P3: mgmt ECHO round-trip OK (attempt %d) — command path is live\n", attempt + 1);

	/* CC_GET_CAPABILITIES (read-only): resp[1..4] flags, [5..8] max_buf, [9] dma engines. */
	status = 0xff;
	ret = agnic_mgmt_cmd(ag, AGNIC_CC_GET_CAPABILITIES, NULL, 0, resp, sizeof(resp), &status);
	if (!ret && status == AGNIC_NOTIF_STATUS_OK) {
		ag->max_buf_size = get_unaligned_le32(&resp[5]);
		dev_info(ag->dev, "P3: caps flags 0x%08x max_buf %u dma_engines %u\n",
			 get_unaligned_le32(&resp[1]), ag->max_buf_size, resp[9]);
	} else {
		ag->max_buf_size = 2048;
	}
	return 0;
}

void agnic_mgmt_teardown(struct agnic *ag)
{
	if (!ag->cmd_ring.mem.va)
		return;
	if (ag->mgmt_inited) {
		ag->mgmt_inited = false;
		cancel_delayed_work_sync(&ag->drain_work);
	}
	/* The device may still poll the rings; leave HOST_MGMT_READY as-is (no FLR).
	 * Freeing here is safe only because the driver is detaching the whole device. */
	agnic_dma_free(ag, &ag->notif_ring.mem);
	agnic_dma_free(ag, &ag->cmd_ring.mem);
	dev_info(ag->dev, "P3: mgmt channel torn down\n");
}
