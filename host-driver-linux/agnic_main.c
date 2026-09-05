// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * mamoru-agnic: clean-room Linux driver for the Marvell AGNIC on the Sophos
 * XGS 116 CN9130 NPU (PCIe PF 11ab:7080). Transcribed from the proven BSD-2
 * FreeBSD if_agnic driver.
 *
 * Milestone P0-P2 (read-only, no NPU writes, NEVER an FLR):
 *   P0  bind the 7080 PF, print identity.
 *   P1  map BAR0/2/4, set busmaster, 36-bit DMA mask.
 *   P2r read the NPU-published barmap descriptor in BAR2, verify cookie/version,
 *       resolve the five facility windows, verify the CTRL cookie, and read the
 *       GIU config_mem status (+ device MAC if DEV_READY). Pure MMIO reads —
 *       proves the transcribed ABI against live hardware with zero risk to the
 *       running NPU firmware. The handshake WRITES (HOST_INIT/HOST_ALIVE, GIU
 *       rings, CC_PF_*) land in later milestones.
 *
 * HARD RULE: this driver must never reset/FLR the device. An FLR nukes the live
 * NPU firmware and the front-panel data plane. There are deliberately no
 * pci_reset_function()/pcie_flr() calls anywhere.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/crc32.h>
#include <linux/dmi.h>
#include "agnic.h"

/* Bring up the mvmgmt0 host<->NPU management netdev after attach. OFF by default:
 * the front-panel datapath rides the GIU rings, not mvmgmt0 (proven with mvmgmt=0),
 * and the NPU auto-starts its own data plane — so mvmgmt0 is unused. Keeping it
 * unregistered also keeps it out of /sys/class/net, so it never surfaces as an
 * operator interface in Mamoru (owner: hide the mgmt interface without a Mamoru-side
 * exception — the built-in appliance driver takes no insmod args, so this default is
 * what ships). Pass `mvmgmt=1` to opt in for host<->NPU pcinet debugging. */
static bool mvmgmt;
module_param(mvmgmt, bool, 0444);
MODULE_PARM_DESC(mvmgmt, "bring up the mvmgmt0 host<->NPU management netdev (default off)");

/* Bring up the P3 GIU management channel (rings + HOST_MGMT_READY + CC_PF_*).
 * On by default; needs dp_fwd/nmp running on the NPU to reach DEV_MGMT_READY. */
static bool p3 = true;
module_param(p3, bool, 0444);
MODULE_PARM_DESC(p3, "bring up the P3 GIU management channel (default on)");

/* Bring up the P3b/P4 GIU datapath (rings + CC_PF_* LIF config + ENABLE) after
 * the mgmt channel is live. On by default; needs the NPU dp app servicing CC_PF_*. */
static bool dp = true;
module_param(dp, bool, 0444);
MODULE_PARM_DESC(dp, "bring up the P3b/P4 GIU datapath after the mgmt channel (default on)");

/* Which per-unit source seeds the front-panel port MACs. See agnic_port_mac_base(). */
static int mac_src;
module_param(mac_src, int, 0444);
MODULE_PARM_DESC(mac_src, "port MAC source: 0=DMI then NPU device MAC, 1=NPU device MAC first, 2=fixed 02:81:00:00:00:0N (default 0)");

const char *agnic_facility_name(enum agnic_facility f)
{
	static const char * const names[AGNIC_FAC_COUNT] = {
		[AGNIC_FAC_CONTROL]     = "CONTROL",
		[AGNIC_FAC_MGMT_NETDEV] = "MGMT_NETDEV",
		[AGNIC_FAC_NW_AGENT]    = "NW_AGENT",
		[AGNIC_FAC_RPC]         = "RPC",
		[AGNIC_FAC_GIU]         = "GIU",
	};

	return (f < AGNIC_FAC_COUNT && names[f]) ? names[f] : "?";
}

/*
 * P2r: locate + validate the barmap descriptor the NPU publishes in BAR2, resolve
 * each facility to (BAR, absolute-offset), and sanity-read the CTRL + GIU windows.
 * Read-only.
 */
static int agnic_read_barmap(struct agnic *ag)
{
	resource_size_t bar2_len = ag->bar_len[AGNIC_BAR_CTRL];
	u32 barmap_off = AGNIC_BARMAP_OFF(bar2_len);
	u32 tail_base = AGNIC_BAR2_TAIL_OFF(bar2_len);
	u32 version, cookie;
	int i;

	version = agnic_rd(ag, AGNIC_BAR_CTRL, barmap_off + AGNIC_BARMAP_VERSION_OFF);
	cookie  = agnic_rd(ag, AGNIC_BAR_CTRL, barmap_off + AGNIC_BARMAP_COOKIE_OFF);

	if (cookie != AGNIC_BARMAP_COOKIE) {
		dev_err(ag->dev,
			"P2: bad barmap cookie 0x%08x @BAR2+0x%x (expected 0x%08x) — NPU firmware not up?\n",
			cookie, barmap_off, AGNIC_BARMAP_COOKIE);
		return -ENODEV;
	}
	ag->barmap_version = version;
	if (version != AGNIC_BARMAP_VERSION)
		dev_warn(ag->dev,
			 "P2: barmap version 0x%08x != expected 0x%08x — continuing (ABI may differ)\n",
			 version, AGNIC_BARMAP_VERSION);
	dev_info(ag->dev, "P2: barmap OK @BAR2+0x%x (cookie 0x%08x, version 0x%08x)\n",
		 barmap_off, cookie, version);

	/* Resolve the facility map, indexed by facility TYPE (the barmap entries are
	 * NOT in type order — entry[i].type is the real facility). BAR0 offsets are
	 * absolute; BAR2 offsets are relative to the facility tail base. */
	for (i = 0; i < AGNIC_FAC_COUNT; i++) {
		ag->fac_bar[i] = -1;
		ag->fac_off[i] = 0;
	}
	for (i = 0; i < AGNIC_FAC_COUNT; i++) {
		u32 ent = barmap_off + AGNIC_BARMAP_FACMAP_OFF + i * AGNIC_FACMAP_ENTRY_SIZE;
		u32 fbar = agnic_rd(ag, AGNIC_BAR_CTRL, ent + 0);
		u32 ftype = agnic_rd(ag, AGNIC_BAR_CTRL, ent + 4);
		u32 foff = agnic_rd(ag, AGNIC_BAR_CTRL, ent + 8);
		u32 fsize = agnic_rd(ag, AGNIC_BAR_CTRL, ent + 12);
		int hbar;
		u32 hoff;

		if (fsize == 0)
			continue;
		if (ftype >= AGNIC_FAC_COUNT) {
			dev_warn(ag->dev, "P2: facility entry %d has unknown type %u\n", i, ftype);
			continue;
		}
		if (fbar == AGNIC_SHM_BAR0) {
			hbar = AGNIC_BAR_GIU;
			hoff = foff;
		} else if (fbar == AGNIC_SHM_BAR2) {
			hbar = AGNIC_BAR_CTRL;
			hoff = tail_base + foff;
		} else {
			dev_warn(ag->dev, "P2: facility %u has unknown bar %u\n", ftype, fbar);
			continue;
		}
		ag->fac_bar[ftype] = hbar;
		ag->fac_off[ftype] = hoff;
		dev_info(ag->dev,
			 "P2:   %-11s (type %u) -> BAR%d + 0x%06x (size 0x%x)\n",
			 agnic_facility_name(ftype), ftype, hbar, hoff, fsize);
	}

	/* Verify the CTRL cookie (facility handshake mailbox). */
	if (ag->fac_bar[AGNIC_FAC_CONTROL] >= 0) {
		u32 ccookie = agnic_rd(ag, ag->fac_bar[AGNIC_FAC_CONTROL],
				       ag->fac_off[AGNIC_FAC_CONTROL] + AGNIC_CTRL_COOKIE_OFF);
		u32 hs = agnic_rd(ag, ag->fac_bar[AGNIC_FAC_CONTROL],
				  ag->fac_off[AGNIC_FAC_CONTROL] + AGNIC_CTRL_HANDSHAKE_OFF);

		dev_info(ag->dev, "P2: CTRL cookie 0x%08x (expected 0x%08x), handshake 0x%08x [%s%s%s%s]\n",
			 ccookie, AGNIC_FACILITY_COOKIE, hs,
			 (hs & CTRL_FCLT_TRGT_INIT) ? "TRGT_INIT " : "",
			 (hs & CTRL_FCLT_HOST_INIT) ? "HOST_INIT " : "",
			 (hs & CTRL_FCLT_TRGT_H2T_DBELL) ? "H2T_DBELL " : "",
			 (hs & CTRL_FCLT_HOST_ALIVE) ? "HOST_ALIVE" : "");
		if (ccookie != AGNIC_FACILITY_COOKIE)
			dev_warn(ag->dev, "P2: CTRL cookie mismatch — facility not stamped yet\n");
	} else {
		dev_warn(ag->dev, "P2: CONTROL facility absent in barmap\n");
	}

	/* Read the GIU config_mem status (+ MAC if the device is ready). */
	if (ag->fac_bar[AGNIC_FAC_GIU] >= 0) {
		int gbar = ag->fac_bar[AGNIC_FAC_GIU];
		u32 goff = ag->fac_off[AGNIC_FAC_GIU];
		u32 status = agnic_rd(ag, gbar, goff + AGNIC_GIU_STATUS_OFF);

		ag->dev_ready = !!(status & AGNIC_CFG_STATUS_DEV_READY);
		dev_info(ag->dev, "P2: GIU config_mem status 0x%08x [%s%s%s]\n", status,
			 (status & AGNIC_CFG_STATUS_DEV_READY) ? "DEV_READY " : "",
			 (status & AGNIC_CFG_STATUS_HOST_MGMT_READY) ? "HOST_MGMT_READY " : "",
			 (status & AGNIC_CFG_STATUS_DEV_MGMT_READY) ? "DEV_MGMT_READY" : "");
		if (ag->dev_ready) {
			agnic_rd_mac(ag, gbar, goff + AGNIC_GIU_MAC_OFF, ag->mac);
			dev_info(ag->dev, "P2: device MAC %pM\n", ag->mac);
		}
	} else {
		dev_warn(ag->dev, "P2: GIU facility absent in barmap\n");
	}

	return 0;
}

/*
 * Re-sample the NPU-published device MAC in GIU config_mem.
 *
 * agnic_read_barmap() reads it once, in probe. Probe runs at device_initcall, seconds
 * into boot, and normally beats the NPU's ~60s boot — so DEV_READY is clear there and
 * ag->mac reads back all-zero from the devm_kzalloc'd struct. By P4 the mgmt handshake
 * has completed, which means the NPU is up, so this is the first point at which the
 * field is worth reading.
 */
static void agnic_refresh_dev_mac(struct agnic *ag)
{
	int gbar = ag->fac_bar[AGNIC_FAC_GIU];
	u32 goff, status;

	if (gbar < 0)
		return;
	goff = ag->fac_off[AGNIC_FAC_GIU];
	status = agnic_rd(ag, gbar, goff + AGNIC_GIU_STATUS_OFF);
	ag->dev_ready = !!(status & AGNIC_CFG_STATUS_DEV_READY);
	if (ag->dev_ready)
		agnic_rd_mac(ag, gbar, goff + AGNIC_GIU_MAC_OFF, ag->mac);
}

/* Factory placeholders. A board that reports one of these reports it on every unit, so
 * accepting one would hand the whole installed base the same address while looking like
 * a success. Matched case-insensitively and by prefix, because vendors vary the case and
 * append junk ("Default String", "System Serial Number  "). */
static const char * const agnic_dmi_junk[] = {
	"none", "default string", "not specified", "system serial number",
	"to be filled", "o.e.m.", "0123456789", "unknown", "n/a",
	"00000000-0000-0000-0000-000000000000",
	"ffffffff-ffff-ffff-ffff-ffffffffffff",
};

static bool agnic_dmi_usable(const char *s)
{
	size_t i;

	if (!s)
		return false;
	while (*s == ' ')
		s++;
	if (strlen(s) < 4)
		return false;
	for (i = 0; i < ARRAY_SIZE(agnic_dmi_junk); i++)
		if (!strncasecmp(s, agnic_dmi_junk[i], strlen(agnic_dmi_junk[i])))
			return false;
	return true;
}

/*
 * 24 bits of per-unit entropy from the x86 board's own DMI, which needs nothing from the
 * NPU.
 *
 * UUID first: SMBIOS requires it to be unique per unit, and it is the field appliance
 * vendors are least likely to stub. The serials are tried after it, not before, so that
 * a board with a placeholder serial does not shadow a good UUID — the loop takes the
 * first USABLE field, and ordering decides which that is.
 */
static bool agnic_dmi_entropy(u32 *out)
{
	static const int ids[] = { DMI_PRODUCT_UUID, DMI_PRODUCT_SERIAL, DMI_BOARD_SERIAL };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		const char *s = dmi_get_system_info(ids[i]);

		if (!agnic_dmi_usable(s))
			continue;
		*out = crc32_le(~0U, (const u8 *)s, strlen(s)) & 0xffffff;
		return true;
	}
	return false;
}

/*
 * Base address for the front-panel port MACs: 02:81:<24 bits per unit>:<port>.
 *
 * Locally administered throughout. This project owns no OUI, so a globally-unique
 * address would be a claim it cannot make — including when the source is a real
 * hardware address, of which the NPU publishes exactly one for the whole trunk while
 * nine distinct port addresses are needed.
 *
 * Two candidate sources, and mac_src picks the order:
 *   - the x86 board's DMI serial or UUID, hashed. Per-unit by SMBIOS convention, and
 *     readable without the NPU;
 *   - the NPU-published device MAC (GIU config_mem + AGNIC_GIU_MAC_OFF), low three
 *     octets, so the derived address stays legibly related to the hardware one.
 * If neither yields anything the base is all-zero, which reproduces the historical
 * fixed 02:81:00:00:00:0N byte for byte. That floor means this can never fail.
 *
 * DMI is preferred by default because the device MAC's provenance is not yet
 * established: nothing in npu-firmware/ writes that field, and its sibling in the
 * pcinet descriptor (PC_CFG_REMOTE_MAC, the NPU's own address) is a firmware constant
 * that reads identically on every unit. A source that is constant across appliances is
 * worse than no source at all here, because it looks like identity and is not. Once
 * `P4: port MAC base ... (NPU device MAC)` has been compared across two units and the
 * values differ, `mac_src=1` makes it the preferred source.
 */
void agnic_port_mac_base(struct agnic *ag, u8 base[ETH_ALEN])
{
	u32 dev_ent = 0, dmi_ent = 0, ent;
	bool dev_ok, dmi_ok;
	const char *src;
	int mode = mac_src;

	if (mode < 0 || mode > 2) {
		dev_warn(ag->dev, "P4: mac_src=%d out of range; using 0\n", mode);
		mode = 0;
	}

	/* Always re-sample: it is a handful of MMIO reads, and ag->dev_ready is reported
	 * below even when the value itself is not used. */
	agnic_refresh_dev_mac(ag);

	dev_ok = is_valid_ether_addr(ag->mac);
	if (dev_ok)
		dev_ent = (ag->mac[3] << 16) | (ag->mac[4] << 8) | ag->mac[5];
	dmi_ok = (mode != 2) && agnic_dmi_entropy(&dmi_ent);

	if (mode == 2) {
		ent = 0;
		src = "fixed (mac_src=2)";
	} else if (mode == 1 && dev_ok) {
		ent = dev_ent;
		src = "NPU device MAC";
	} else if (dmi_ok) {
		ent = dmi_ent;
		src = "host DMI";
	} else if (dev_ok) {
		ent = dev_ent;
		src = "NPU device MAC";
	} else {
		ent = 0;
		src = "no per-unit source; fixed fallback";
	}

	base[0] = 0x02;			/* locally administered, unicast */
	base[1] = 0x81;			/* mnemonic echo of the pport tag base */
	base[2] = (ent >> 16) & 0xff;
	base[3] = (ent >> 8) & 0xff;
	base[4] = ent & 0xff;
	base[5] = 0;			/* caller stamps the port number */

	/* dev_ready is reported because it is the difference between "the device MAC was
	 * rejected" and "the device MAC was never readable". Without it, a unit that falls
	 * back cannot be told apart from one whose NPU had not set DEV_READY yet. */
	if (ent) {
		dev_info(ag->dev, "P4: port MAC base %pM (port in last octet) from %s [DEV_READY=%d]\n",
			 base, src, ag->dev_ready);
	} else {
		/* Not a success: every unit that lands here carries the same nine addresses,
		 * which is the collision this derivation exists to remove. */
		dev_warn(ag->dev,
			 "P4: port MAC base %pM (port in last octet) — %s; NOT unique to this appliance [DEV_READY=%d]\n",
			 base, src, ag->dev_ready);
	}
}

/*
 * Async front-panel bring-up. Built-in, agnic_probe runs at device_initcall — seconds
 * into boot — and can beat the NPU's ~60s boot, so the DEV_MGMT_READY handshake must not
 * be a one-shot wait (a lost race would leave the appliance with no front-panel ports,
 * permanently, until reboot). This worker completes the mgmt handshake as soon as the NPU
 * answers, retrying every second until then (HOST_MGMT_READY is already asserted, so it is
 * boot-order agnostic), and only then brings up the GIU datapath + the port1..port9 netdevs.
 */
static void agnic_bringup_work_fn(struct work_struct *w)
{
	struct agnic *ag = container_of(to_delayed_work(w), struct agnic, bringup_work);
	int ret;

	if (!READ_ONCE(ag->bringup_active))
		return;				/* detach in progress */

	ret = agnic_mgmt_finish(ag);
	if (ret == -EAGAIN) {
		/* NPU (dp_fwd) not up yet — the front-panel ports are load-bearing, so
		 * keep polling. Log once, then a periodic reminder so it isn't silent. */
		if (ag->bringup_tries == 0)
			dev_info(ag->dev, "P3: awaiting the NPU dp_fwd (DEV_MGMT_READY) before the datapath...\n");
		else if (ag->bringup_tries % 30 == 0)
			dev_info(ag->dev, "P3: still awaiting the NPU dp_fwd (~%us)...\n", ag->bringup_tries);
		ag->bringup_tries++;
		if (READ_ONCE(ag->bringup_active))
			queue_delayed_work(ag->bringup_wq, &ag->bringup_work, msecs_to_jiffies(1000));
		return;
	}
	if (ret) {
		dev_warn(ag->dev, "P3: mgmt handshake failed (%d) after DEV_MGMT_READY; no front-panel datapath\n",
			 ret);
		return;				/* hard error — stop retrying */
	}

	/* Mgmt command path is live. Bring up the datapath + front-panel netdevs (gated
	 * on `dp`; p3-only is a mgmt-channel diagnostic mode with no datapath). */
	if (!dp)
		return;
	ret = agnic_txrx_bringup(ag);
	if (ret) {
		dev_warn(ag->dev, "P3b: datapath bring-up failed (%d); attached without it\n", ret);
		return;
	}
	agnic_datapath_start(ag);
}

static int agnic_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct agnic *ag;
	void __iomem * const *tbl;
	int ret;

	ag = devm_kzalloc(&pdev->dev, sizeof(*ag), GFP_KERNEL);
	if (!ag)
		return -ENOMEM;
	ag->pdev = pdev;
	ag->dev = &pdev->dev;
	pci_set_drvdata(pdev, ag);

	/* P0: bind + identity. NO reset/FLR path — see the file banner. */
	dev_info(&pdev->dev, "P0: Marvell AGNIC GIU-NIC PF %04x:%04x (rev %02x)\n",
		 pdev->vendor, pdev->device, pdev->revision);

	/* P1: enable (never resets), busmaster, map BAR0/2/4, 36-bit DMA. */
	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "pcim_enable_device\n");

	ret = pcim_iomap_regions(pdev, AGNIC_BAR_MASK, AGNIC_DRV_NAME);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "mapping BAR0/2/4\n");
	tbl = pcim_iomap_table(pdev);
	if (!tbl)
		return -ENOMEM;
	ag->bar[AGNIC_BAR_GIU]   = tbl[AGNIC_BAR_GIU];
	ag->bar[AGNIC_BAR_CTRL]  = tbl[AGNIC_BAR_CTRL];
	ag->bar[AGNIC_BAR_DBELL] = tbl[AGNIC_BAR_DBELL];
	ag->bar_len[AGNIC_BAR_GIU]   = pci_resource_len(pdev, AGNIC_BAR_GIU);
	ag->bar_len[AGNIC_BAR_CTRL]  = pci_resource_len(pdev, AGNIC_BAR_CTRL);
	ag->bar_len[AGNIC_BAR_DBELL] = pci_resource_len(pdev, AGNIC_BAR_DBELL);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(AGNIC_DMA_BITS));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "no %d-bit DMA\n", AGNIC_DMA_BITS);

	pci_set_master(pdev);
	dev_info(&pdev->dev, "P1: BAR0 %lluK, BAR2 %lluM, BAR4 %lluM mapped; %d-bit DMA; busmaster on\n",
		 (u64)ag->bar_len[AGNIC_BAR_GIU] >> 10,
		 (u64)ag->bar_len[AGNIC_BAR_CTRL] >> 20,
		 (u64)ag->bar_len[AGNIC_BAR_DBELL] >> 20, AGNIC_DMA_BITS);

	/* P2r: read + verify the NPU's published contract (read-only). */
	ret = agnic_read_barmap(ag);
	if (ret) {
		pci_clear_master(pdev);
		return ret;
	}

	dev_info(&pdev->dev, "attached (P0-P2). barmap + facilities OK.\n");

	/* P3: publish the GIU mgmt rings + assert HOST_MGMT_READY (persistent), then hand
	 * off to an async worker that completes the handshake and brings up the datapath
	 * whenever the NPU's dp_fwd answers DEV_MGMT_READY — in any host/NPU boot order. A
	 * synchronous one-shot wait here would be fatal when built-in (probe beats the NPU
	 * boot). Best-effort: a publish failure leaves P0-P2 intact. */
	INIT_DELAYED_WORK(&ag->bringup_work, agnic_bringup_work_fn);
	if (p3) {
		ret = agnic_mgmt_publish(ag);
		if (ret) {
			dev_warn(&pdev->dev, "P3: mgmt rings not published (%d); attached without the datapath\n", ret);
		} else {
			ag->bringup_wq = alloc_ordered_workqueue("agnic-bringup", 0);
			if (ag->bringup_wq) {
				WRITE_ONCE(ag->bringup_active, true);
				queue_delayed_work(ag->bringup_wq, &ag->bringup_work, 0);
			} else {
				dev_warn(&pdev->dev, "P3: no bring-up workqueue; front-panel ports unavailable\n");
			}
		}
	}

	/* P5: bring up mvmgmt0 — the management link to the NPU (to start dp_fwd, etc.).
	 * Best-effort: a failure here doesn't unbind the device (P0-P2 stays valid). */
	if (mvmgmt) {
		ret = agnic_pcinet_bringup(ag);
		if (ret)
			dev_warn(&pdev->dev, "P5: mvmgmt0 bring-up failed (%d); attached without it\n", ret);
	}
	return 0;
}

static void agnic_remove(struct pci_dev *pdev)
{
	struct agnic *ag = pci_get_drvdata(pdev);

	/* Stop the async bring-up retrier FIRST so it cannot start the datapath while we
	 * tear it down. An in-flight instance runs to completion under the sync cancel;
	 * the entry check makes any straggler re-queue a no-op, and destroy_workqueue
	 * drains it. bringup_work is always INIT'd (even for p3=0), so this is safe. */
	WRITE_ONCE(ag->bringup_active, false);
	cancel_delayed_work_sync(&ag->bringup_work);
	if (ag->bringup_wq) {
		destroy_workqueue(ag->bringup_wq);
		ag->bringup_wq = NULL;
	}

	agnic_pcinet_teardown(ag);
	/* Datapath before mgmt: CC_PF_DISABLE rides the mgmt command path + drainer. */
	agnic_txrx_teardown(ag);
	agnic_mgmt_teardown(ag);
	/* Managed resources (pcim_*) are released automatically. We only drop
	 * busmaster; we deliberately never reset/FLR the NPU. */
	pci_clear_master(pdev);
	dev_info(&pdev->dev, "detached\n");
}

static const struct pci_device_id agnic_id_table[] = {
	{ PCI_DEVICE(AGNIC_VENDOR_MARVELL, AGNIC_DEVICE_PF) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, agnic_id_table);

static struct pci_driver agnic_driver = {
	.name     = AGNIC_DRV_NAME,
	.id_table = agnic_id_table,
	.probe    = agnic_probe,
	.remove   = agnic_remove,
	/* No .sriov_configure, no reset/err_handler that could FLR the PF. */
};

module_pci_driver(agnic_driver);

MODULE_DESCRIPTION("Clean-room Linux driver for the Marvell AGNIC (CN9130 NPU) on Sophos XGS 116");
MODULE_AUTHOR("Mamoru");
MODULE_LICENSE("Dual MIT/GPL");
