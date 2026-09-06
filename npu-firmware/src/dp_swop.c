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

/* The board's SFP cage descriptor and its sideband GPIO reads. A cage is reached over a
 * completely different bus from this file's SMI, so it lives in its own pair — but it is
 * #included at the FOOT of this file, not compiled separately, because forwarder.c compiles
 * THIS file as a single translation unit (build_fwd.sh:14-17, docs/LICENSING.md). */
#include "dp_sff.h"

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

	case DP_SWOP_OP_SFP: {
		const struct dp_sff_cage *cage;
		struct dp_sff_xchk xchk;
		uint8_t raw = 0, ok = 0;
		uint8_t implemented = 0, active_low = 0, valid = 0, logical = 0;

		/* IDENTITY FIRST, AND ON EVERY PATH INCLUDING THE ERROR ONES. These four fields
		 * ARE the echo defence, not decoration: they sit at offsets 12..17, past the end
		 * of the TWELVE-byte request, so a pre-D84 NPU echoing the request verbatim has
		 * no byte to supply them with and the host's verdict lands on NOMARK instead of
		 * on the manufactured OK that the PORTS echo produced. The wire contract requires
		 * mark/port/page/cage_map on E_NOCAGE and E_PAGE too, which is why they are
		 * written before the first refusal rather than on the success path. */
		resp.sfp.port = req.dev;
		resp.sfp.page = req.reg;
		resp.sfp.mark = DP_SWOP_SFP_MARK;
		resp.sfp.cage_map = dp_sff_cage_map();
		/* resp.count is left 0 DELIBERATELY. It is one of the two fields the old echo
		 * aliased onto (resp.count <- req.reg), so OP_SFP gives it no job at all: nothing
		 * in the host verdict reads it, and any later use of it has to survive the echo
		 * analysis again rather than inherit a pass. */

		/* ORDER: request VALIDITY, then request SUPPORT, then BOARD fact. The same
		 * discipline OP_WRITE follows, for the same reason -- "0x1b is not a port
		 * device", "phase-A firmware cannot serve page A0" and "port 3 has no cage" are
		 * three different answers, and a caller debugging a refusal needs to know which.
		 *
		 * THE LEGAL SET HERE IS NARROWER THAN dev_is_legal()'s, on purpose. Global1 and
		 * Global2 are real, readable devices, but they are not PORTS and cannot own a
		 * front-panel cage; the contract says a dev outside 0x00..0x0a answers E_DEV.
		 * Calling dev_is_legal() would have let 0x1b through to E_NOCAGE -- a board fact
		 * about a device that has no front panel to have a cage on. */
		if (req.dev > DP_SWOP_PORT_DEV_MAX) {
			resp.status = DP_SWOP_E_DEV;
			break;
		}
		if (req.reg != DP_SWOP_SFP_PAGE_PINS) {
			/* PAGE_A0 and PAGE_A2 land here: phase-A firmware REFUSES the page it
			 * cannot serve. Never a zero-filled page -- SFF-8472 assigns meanings to
			 * 0x00, so a zeroed page decodes as a plausible, wrong, MODULE-SHAPED
			 * answer. That is the absence contract sfp-rate-read.sh pre-registered. */
			resp.status = DP_SWOP_E_PAGE;
			break;
		}
		/* req.val is the page BYTE OFFSET, and phase A ignores it. Written down because
		 * "ignored" has to be a decision the code shows rather than an omission a reader
		 * has to infer -- and because phase B is where it starts mattering. */

		cage = dp_sff_cage_for_dev(req.dev);
		if (!cage) {
			/* ABSENCE CASE (a). implemented and valid stay 0 -- but for a DIFFERENT
			 * reason than in case (b), and the status is what carries the difference. */
			resp.status = DP_SWOP_E_NOCAGE;
			break;
		}

		implemented = dp_sff_implemented(cage);
		active_low = dp_sff_active_low(cage);
		resp.sfp.implemented = implemented;
		resp.sfp.active_low = active_low;

		st = dp_sff_read_pins(cage, &raw, &ok);
		if (st != DP_SWOP_OK) {
			/* The controller itself could not be resolved BY LABEL. `implemented`
			 * stays SET -- the board still wires those pins -- while valid, raw and
			 * logical stay 0, so the reply says "these four exist and I measured none
			 * of them" rather than shipping four zeros that decode as a seated,
			 * silent, perfectly healthy module. */
			resp.status = st;
			break;
		}

		/* A pin this firmware does not offer as a measurement may not be reported. */
		ok = (uint8_t)(ok & implemented);
		if (!ok) {
			/* The chip resolved but NOT ONE line could be read. This is the same
			 * discipline npu_switch_ports follows when it refuses to render an
			 * all-zero table: a silently-failed scan and a genuinely quiet board
			 * read identically, so a scan that read nothing must not report
			 * success. Louder than an OK carrying valid = 0, and no less true. */
			resp.status = DP_SWOP_E_GPIO;
			break;
		}
		raw = (uint8_t)(raw & ok);
		logical = (uint8_t)((raw ^ active_low) & ok);
		valid = ok;

		/* ABSENCE CASE (b) -- a cage with nothing seated. rx_los, tx_fault and tx_disable
		 * are pins of a MODULE, and with no module the level the board floats to is not a
		 * measurement of light; reporting it as a zero reading is exactly the
		 * manufactured zero this opcode exists to prevent. Their `valid` bits clear while
		 * `implemented` stays set, and that is what distinguishes (b) from (c).
		 *
		 * THE SAME APPLIES WHEN PRESENT ITSELF DID NOT READ. An rx_los level that cannot
		 * be attributed to a seated module is not attributable at all, so "we could not
		 * tell" is folded in here rather than being allowed to fall through to the
		 * seated case by default. */
		if (!(valid & DP_SWOP_SFP_PIN_PRESENT) ||
		    !(logical & DP_SWOP_SFP_PIN_PRESENT))
			valid = (uint8_t)(valid & (uint8_t)~(unsigned)(DP_SWOP_SFP_PIN_RX_LOS |
								      DP_SWOP_SFP_PIN_TX_FAULT |
								      DP_SWOP_SFP_PIN_TX_DISABLE));

		/* THE A2 CROSS-CHECK. Nothing above this line establishes that the cage pads are
		 * actually in GPIO FUNCTION, and on a pad whose function has not been established
		 * NEITHER LEVEL IS EVIDENCE -- a pad held in a peripheral function reads LOW, and
		 * an open-drain i2c pad idling on its pull-up reads HIGH, which is exactly what an
		 * asserted TX_DISABLE looks like. Two of these four pins are readable a SECOND way,
		 * over a different controller, from the module itself (SFF-8472 A2h byte 110), and
		 * a disagreement between the two is the muxed-or-dead-pad signature.
		 *
		 * A DISAGREEING PIN HAS NOT BEEN VALIDLY MEASURED, so its `valid` bit clears while
		 * `implemented` stays set -- the contract's existing shape for wired-but-unmeasured,
		 * which the host already renders distinctly from a pin reading 0. NO WINNER IS
		 * PICKED between the two transports, exactly as the sysfs row one layer up refuses
		 * a row whose two sides disagree rather than choosing one.
		 *
		 * ORDER: AFTER the (b) clamp, deliberately. An empty cage has already dropped
		 * rx_los and tx_disable from `valid`, so `want` is then empty and the bus is not
		 * touched at all -- there is no module to answer, and asking would only produce a
		 * failure that has to be reasoned about.
		 *
		 * THREE STATES, and only one of them changes anything here. `agreed` and `nocheck`
		 * (no module diagnostics, a bus that did not answer, or a gpiochip that shares an
		 * adapter with the EEPROM and is therefore not a second transport) both leave
		 * `valid` exactly as it was. An i2c failure is NOT a disagreement. */
		dp_sff_cross_check(cage, logical, (uint8_t)(valid & DP_SFF_XCHK_PINS), &xchk);
		valid = (uint8_t)(valid & (uint8_t)~(unsigned)xchk.disagreed);

		resp.sfp.valid = valid;
		resp.sfp.logical = logical;
		resp.sfp.raw = raw;
		/* page_off and page_len stay 0: phase A carries no page bytes, and an EMPTY RANGE
		 * is not a zero READING. data[] stays zeroed by the memset at the top. */
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

/*
 * SINGLE TRANSLATION UNIT, matching the arrangement one layer up: forwarder.c #includes THIS
 * file (build_fwd.sh:14-17, docs/LICENSING.md), so the SFP/GPIO half is #included HERE rather
 * than compiled and linked separately. dp_fwd therefore gains no object file and no link step,
 * and dp_swop_test.c's existing command line -- two .c files -- keeps working unchanged.
 *
 * It is at the FOOT so this file reads as itself first; only dp_sff.h's declarations are needed
 * above, and everything dp_sff.c exposes is prefixed dp_sff_/sff_ so nothing collides in the
 * merged unit.
 */
#include "dp_sff.c"
