/* SPDX-License-Identifier: MIT
 *
 * dp_swop.c — D84 switch-access handler for the 88E6193X behind the CN9130.
 *
 * Direct implementation of the raw orion-SMI, multi-chip SMI-address-2 indirect
 * protocol proven on hardware by switch-init/swmdio.sh. That script is the reference;
 * this is the same sequence in C so it can run inside dp_fwd's management pump in
 * microseconds instead of spawning a `devmem` process per register access.
 *
 * Outer orion SMI word @ 0xf212a200:
 *   data(15:0) phy(20:16) reg(25:21) readop(26) ReadValid(27) busy(28)
 * Inner switch SMI-command word (phy 2, reg 0):
 *   busy(15) mode-c22(12) op(11:10 read=10 write=01) dev(9:5) reg(4:0)
 * Switch SMI data is phy 2, reg 1.
 */

#include "dp_swop.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SMI_PHYS		0xf212a200u
#define SMI_PAGE		(SMI_PHYS & ~0xfffu)	/* 0xf212a000 */
#define SMI_OFF			(SMI_PHYS & 0xfffu)	/* 0x200      */

#define OUTER_BUSY		(1u << 28)
#define OUTER_VALID		(1u << 27)
#define OUTER_READOP		(1u << 26)

/* Pre-composed outer words for the two fixed accesses (phy 2, regs 0 and 1). */
#define OUTER_RD_CMD		0x04020000u	/* read  phy2 reg0 (SMI command) */
#define OUTER_RD_DATA		0x04220000u	/* read  phy2 reg1 (SMI data)    */
#define OUTER_WR_CMD_BASE	0x00020000u	/* write phy2 reg0               */
#define OUTER_WR_DATA_BASE	0x00220000u	/* write phy2 reg1               */

#define INNER_BUSY		0x8000u
#define INNER_C22		0x1000u
#define INNER_OP_READ		0x0800u		/* op = 10 */
#define INNER_OP_WRITE		0x0400u		/* op = 01 */

/* Bounded spins. swmdio.sh uses 200 iterations of a shell loop; each iteration here
 * is a single MMIO read, so this is far tighter in wall-clock while being far more
 * generous in attempts. It MUST stay bounded: this runs on dp_fwd's management pump
 * thread, and an unbounded wait there stalls the host handshake and tears the link
 * down (see forwarder.c's mng_pump_thread comment). */
/* ONE TOTAL BUDGET, not two nested limits. inner_wait() calls outer_wait() inside its
 * loop, so independent bounds multiply: 20000 x 20002 is ~4e8 MMIO accesses per
 * inner_wait, and a single reg_read performs two of those plus two outer_waits. The
 * header claimed "bounded and non-blocking"; the arithmetic did not support it
 * (mamoru-d7, F5). A shared decrementing budget makes the claim true by construction:
 * whatever path is taken, one transaction costs at most SPIN_BUDGET MMIO accesses. */
#define SPIN_BUDGET		60000u

static volatile unsigned char *g_map;	/* mmap'd page, NULL when unmapped */
static int g_fd = -1;

static inline uint32_t smi_rd(void)
{
	return *(volatile uint32_t *)(g_map + SMI_OFF);
}

static inline void smi_wr(uint32_t v)
{
	*(volatile uint32_t *)(g_map + SMI_OFF) = v;
}

int dp_swop_init(void)
{
	void *p;

	if (g_map)
		return 0;			/* idempotent */

	g_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (g_fd < 0)
		return -errno;

	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)SMI_PAGE);
	if (p == MAP_FAILED) {
		int e = -errno;
		close(g_fd);
		g_fd = -1;
		return e;
	}
	g_map = (volatile unsigned char *)p;
	return 0;
}

void dp_swop_fini(void)
{
	if (g_map) {
		munmap((void *)g_map, 0x1000);
		g_map = NULL;
	}
	if (g_fd >= 0) {
		close(g_fd);
		g_fd = -1;
	}
}

/* Wait for the OUTER orion SMI to go idle, spending from the shared budget. */
static int outer_wait(unsigned *budget)
{
	while (*budget) {
		(*budget)--;
		if (!(smi_rd() & OUTER_BUSY))
			return 0;
	}
	return -1;
}

/* Wait for the INNER switch SMI command register to clear its busy bit. Shares the
 * SAME budget as every outer_wait it performs, so the total is bounded once. */
static int inner_wait(unsigned *budget)
{
	while (*budget) {
		smi_wr(OUTER_RD_CMD);
		if (outer_wait(budget) < 0)
			return -1;
		if (!*budget)
			return -1;
		(*budget)--;
		if (!(smi_rd() & INNER_BUSY))
			return 0;
	}
	return -1;
}

/* A device address the switch actually implements. Everything else is refused —
 * a wrong address answers with ReadValid SET and data 0x0000, so issuing it would
 * produce a confident, wrong zero rather than an error. */
static int dev_is_legal(uint8_t dev)
{
	return dev <= DP_SWOP_PORT_DEV_MAX ||
	       dev == DP_SWOP_DEV_GLOBAL1 ||
	       dev == DP_SWOP_DEV_GLOBAL2;
}

static uint8_t reg_read(uint8_t dev, uint8_t reg, uint16_t *out)
{
	unsigned budget = SPIN_BUDGET;
	uint32_t v;

	/* Validate the REQUEST before consulting hardware state. A malformed request is
	 * malformed whether or not the window is mapped, and reporting E_NOMAP for a bad
	 * device address would blame the box for the caller's bug. */
	if (!dev_is_legal(dev))
		return DP_SWOP_E_DEV;
	if (reg > 31)
		return DP_SWOP_E_REG;
	if (!g_map)
		return DP_SWOP_E_NOMAP;

	if (inner_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_CMD_BASE | INNER_BUSY | INNER_C22 | INNER_OP_READ |
	       ((uint32_t)dev << 5) | reg);
	if (outer_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	if (inner_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;

	smi_wr(OUTER_RD_DATA);
	if (outer_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	v = smi_rd();

	/* ReadValid means "the bus answered", NOT "you asked the right thing" — the
	 * 2026-09-01 base-address bug returned valid=1 with data=0x0000. It is still
	 * worth testing, because without it a transaction that never completed is
	 * indistinguishable from a register that genuinely reads zero. */
	if (!(v & OUTER_VALID))
		return DP_SWOP_E_INVALID;

	*out = (uint16_t)(v & 0xffffu);
	return DP_SWOP_OK;
}

/* The WRITE allowlist. Only what D84 actually needs; everything else refused by
 * construction. Globals are readable but NEVER writable — a VTU flush there is a
 * switch-wide blackout, strictly worse than isolating one port.
 *
 * Adding a pair here is a deliberate act and needs a stated reason, because the cost
 * of a wrong entry is a box off the network with a mains cycle as the only recovery. */
int dp_swop_write_allowed(uint8_t dev, uint8_t reg)
{
	if (dev > DP_SWOP_PORT_DEV_MAX)
		return 0;			/* Global1/Global2: read-only, always */
	return reg == DP_SWOP_REG_VLAN_MAP;	/* reg 6 — the per-port VLAN map D84 writes */
}

static uint8_t reg_write(uint8_t dev, uint8_t reg, uint16_t val)
{
	unsigned budget = SPIN_BUDGET;

	if (!dev_is_legal(dev))
		return DP_SWOP_E_DEV;
	if (reg > 31)
		return DP_SWOP_E_REG;
	if (!g_map)
		return DP_SWOP_E_NOMAP;

	if (inner_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_DATA_BASE | val);		/* SMI data first  */
	if (outer_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_CMD_BASE | INNER_BUSY | INNER_C22 | INNER_OP_WRITE |
	       ((uint32_t)dev << 5) | reg);		/* then the command */
	if (outer_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	if (inner_wait(&budget) < 0)
		return DP_SWOP_E_BUSY;
	return DP_SWOP_OK;
}

/*
 * TEST SEAM — compiled out entirely unless DP_SWOP_TEST is defined.
 *
 * The PORTS count logic (how many entries are valid when a scan dies part-way) is pure
 * ENVELOPE logic, and it was the one part of this file no test could reach: every check
 * runs with the window unmapped, so the loop always dies at d=0 and a count between 1
 * and 10 was never produced. A mutation making a genuine partial claim to be a FULL
 * table passed the whole suite (mamoru-d7, F4).
 *
 * This is deliberately NOT a mock of the SMI transaction — that is hardware-proven and
 * a mock would only assert this file agrees with itself. It substitutes reg_read so the
 * COUNT arithmetic above it becomes observable, which is exactly what this file claims
 * to cover.
 *
 * The indirection does not exist in a shipping build: without DP_SWOP_TEST the macro
 * expands to the direct call, so dp_fwd carries no function pointer to divert and no
 * hook to reach. build_fwd.sh never defines it.
 */
#ifdef DP_SWOP_TEST
static uint8_t (*g_reg_read)(uint8_t, uint8_t, uint16_t *) = reg_read;

void dp_swop_test_set_reg_read(uint8_t (*fn)(uint8_t, uint8_t, uint16_t *))
{
	g_reg_read = fn ? fn : reg_read;
}
#define REG_READ(d, r, o)	g_reg_read((d), (r), (o))

/* Same seam for WRITE, and for the same reason one step further on: the read-back
 * VERIFICATION (a write whose stored value differs from the requested one must report
 * E_VERIFY, not success) is pure envelope logic, and it was unreachable because every
 * test runs with the window unmapped — reg_write failed first and the read-back never
 * executed. Substituting reg_write makes the verification observable without mocking
 * the SMI transaction itself, which stays hardware-proven. */
static uint8_t (*g_reg_write)(uint8_t, uint8_t, uint16_t) = reg_write;

void dp_swop_test_set_reg_write(uint8_t (*fn)(uint8_t, uint8_t, uint16_t))
{
	g_reg_write = fn ? fn : reg_write;
}
#define REG_WRITE(d, r, v)	g_reg_write((d), (r), (v))
#else
#define REG_READ(d, r, o)	reg_read((d), (r), (o))
#define REG_WRITE(d, r, v)	reg_write((d), (r), (v))
#endif

int dp_swop_service(const void *req_buf, unsigned req_len, void *resp_buf, unsigned resp_cap)
{
	struct dp_swop_req req;
	struct dp_swop_resp resp;
	uint16_t pre = 0;
	unsigned out_len;
	uint8_t st;

	if (resp_cap < sizeof(resp))
		return -1;			/* cannot even answer; caller's bug */

	memset(&resp, 0, sizeof(resp));
	resp.magic = DP_SWOP_RESP_MAGIC;	/* NEVER the request magic — an echo must be detectable */
	resp.version = DP_SWOP_VERSION;

	if (req_len < sizeof(req)) {
		resp.status = DP_SWOP_E_LEN;
		goto out;
	}
	memcpy(&req, req_buf, sizeof(req));

	/* Echo op back even on failure, so a reply can never be attributed to a
	 * different request than the one that produced it. */
	resp.op = req.op;

	if (req.magic != DP_SWOP_MAGIC || req.version != DP_SWOP_VERSION) {
		resp.status = DP_SWOP_E_MAGIC;
		goto out;
	}

	switch (req.op) {
	case DP_SWOP_OP_READ:
		/* REG_READ, not reg_read. This was the ONE call site in this file that bypassed
		 * the test seam -- every other read and write goes through the macro -- and the
		 * consequence was that OP_READ's SUCCESS path had no coverage at all: with the
		 * window unmapped, every test of a legal address could only ever assert E_NOMAP.
		 * That matters beyond tidiness, because A0.2's hardware negative control (read a
		 * register, hand-write it from the NPU console, read again) is exactly this op. */
		resp.status = REG_READ(req.dev, req.reg, &resp.val);
		break;

	case DP_SWOP_OP_WRITE:
		/* ORDER MATTERS: address VALIDITY first, then write POLICY. They are
		 * different failures and conflating them loses information — "0x0b is not a
		 * device on this switch" and "Global1 is a real device you may not write" are
		 * distinct answers, and a caller debugging a refusal needs to know which.
		 * (The test caught this: an allowlist checked first reported E_WRPERM for
		 * addresses that are simply not devices.) */
		if (!dev_is_legal(req.dev)) {
			resp.status = DP_SWOP_E_DEV;
			break;
		}
		if (req.reg > 31) {
			resp.status = DP_SWOP_E_REG;
			break;
		}
		if (!dp_swop_write_allowed(req.dev, req.reg)) {
			resp.status = DP_SWOP_E_WRPERM;
			break;
		}
		/* PRE-READ. The post-write comparison is blind when the register ALREADY
		 * held the requested value: it succeeds with no evidence the write reached
		 * the intended device. Observing a CHANGE is the only thing that proves the
		 * addressed register responded. (mamoru-43 found this for the zero case —
		 * a wrong-but-legal device returns valid=1 data=0x0000, and reg6=0x000 is a
		 * plausible D84 operation — but it generalises to any already-present value.) */
		st = REG_READ(req.dev, req.reg, &pre);
		if (st != DP_SWOP_OK) {
			resp.status = st;	/* cannot establish a baseline; do not write */
			break;
		}
		st = REG_WRITE(req.dev, req.reg, req.val);
		if (st != DP_SWOP_OK) {
			resp.status = st;
			break;
		}
		/* READ BACK. There is no WriteValid bit, so a returned OK means only that a
		 * transaction completed -- never that the value landed. Reserved, read-only
		 * and self-clearing bits also mean the stored value can legitimately differ
		 * from the one sent. Report what the register NOW READS, and say so. */
		st = REG_READ(req.dev, req.reg, &resp.val);
		if (st != DP_SWOP_OK) {
			resp.status = st;	/* wrote, but cannot confirm */
			break;
		}
		/* Echo the target so a reply can never be attributed to a different write. */
		resp.ports[0].status = ((uint16_t)req.dev << 8) | req.reg;
		resp.ports[0].vlan_map = req.val;	/* what was REQUESTED */
		resp.count = 1;
		if (resp.val != req.val)
			resp.status = DP_SWOP_E_VERIFY;
		else if (pre == req.val)
			/* Agrees, but nothing moved — unverifiable rather than failed. Not an
			 * error: a caller that means to write a value already present may
			 * accept it. It simply must not be told the write was CONFIRMED. */
			resp.status = DP_SWOP_W_NOCHANGE;
		else
			resp.status = DP_SWOP_OK;	/* a real change was observed */
		break;

	case DP_SWOP_OP_PORTS: {
		uint8_t d;

		for (d = 0; d < DP_SWOP_PORT_COUNT; d++) {
			st = REG_READ(d, DP_SWOP_REG_STATUS, &resp.ports[d].status);
			if (st != DP_SWOP_OK) {
				resp.status = st;
				resp.count = d;	/* how many are actually valid */
				goto out;
			}
			st = REG_READ(d, DP_SWOP_REG_VLAN_MAP, &resp.ports[d].vlan_map);
			if (st != DP_SWOP_OK) {
				resp.status = st;
				resp.count = d;
				goto out;
			}
		}
		resp.count = DP_SWOP_PORT_COUNT;
		resp.status = DP_SWOP_OK;
		break;
	}

	default:
		resp.status = DP_SWOP_E_OP;
		break;
	}

out:
	out_len = sizeof(resp);
	memcpy(resp_buf, &resp, out_len);
	return (int)out_len;
}
