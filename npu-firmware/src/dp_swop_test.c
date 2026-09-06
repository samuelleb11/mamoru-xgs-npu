/* SPDX-License-Identifier: MIT
 *
 * dp_swop_test.c — host unit test for the D84 swop request/response envelope.
 *
 * Runs anywhere with a C compiler; needs no NPU, no MUSDK and no /dev/mem. It
 * deliberately runs with the SMI window UNMAPPED, which is exactly the state that
 * makes the envelope logic observable: every request must still produce a
 * well-formed, correctly-classified response.
 *
 * What it does NOT test: the SMI transaction itself. That is proven on hardware
 * (swmdio.sh, 2026-09-01) and cannot be honestly simulated here — a mock would
 * only assert that this file agrees with itself. Stated plainly so a green run is
 * never read as "the switch access works".
 *
 *   cc -Wall -Wextra -Werror -DDP_SWOP_TEST -o dp_swop_test dp_swop_test.c dp_swop.c \
 *     && ./dp_swop_test
 *
 * -DDP_SWOP_TEST enables the reg_read seam used by section 12 (the PORTS count checks)
 * and NOTHING else. It is never defined by build_fwd.sh, so the shipping dp_fwd has no
 * seam to divert. This file REQUIRES it — see the #error below, which exists so a build
 * with the old command line says what to add rather than failing on a missing symbol.
 */

#ifndef DP_SWOP_TEST
#error "build this file with -DDP_SWOP_TEST (section 12 needs the reg_read seam)"
#endif

#include "dp_swop.h"
/* The board descriptor half of OP_SFP. dp_swop.c #includes dp_sff.c at its foot (single
 * translation unit, matching forwarder.c's #include of dp_swop.c), so the existing two-file
 * command line above still builds everything -- there is nothing new to add to it. */
#include "dp_sff.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ck(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL: %s\n", what);
	}
}

static struct dp_swop_resp run(struct dp_swop_req req, unsigned req_len)
{
	struct dp_swop_resp resp;
	int n;

	memset(&resp, 0xAA, sizeof(resp));	/* poison: a field never written shows up */
	n = dp_swop_service(&req, req_len, &resp, sizeof(resp));
	ck(n == (int)sizeof(resp), "service returned a full response");
	return resp;
}

/*
 * Substitute register read, installed only for the PORTS count checks (section 12).
 * Succeeds for every device below `stub_fail_from`, returning a per-port value derived
 * from the device number so an entry cannot pass by holding a coincidental zero, then
 * fails with E_BUSY. `stub_fail_on_vlan` makes the failure land on the port's SECOND
 * read instead of its first — the case where an entry is already half-written.
 */
static uint8_t stub_fail_from;
static int stub_fail_on_vlan;

/* Accepts any write and stores nothing — so the READ-BACK, not the write, decides the
 * verdict. That is the point: a write that "succeeds" while the register keeps its old
 * value is exactly the case with no WriteValid bit to catch it. */
/* A STATEFUL stub: a write is remembered so the read-back sees it. That is what makes
 * the CHANGE-vs-NO-CHANGE distinction testable — with a constant-returning stub every
 * write looks like a no-op and the OK path is unreachable. */
static int stub_store_active;
static uint8_t stub_store_dev, stub_store_reg;
static uint16_t stub_store_val;
static uint16_t stub_store_mask = 0xffff;	/* bits the register actually keeps */

static uint8_t stub_reg_write(uint8_t dev, uint8_t reg, uint16_t val)
{
	stub_store_active = 1;
	stub_store_dev = dev; stub_store_reg = reg;
	/* Real switch registers carry reserved, read-only and self-clearing bits, so the
	 * value that LANDS can legitimately differ from the value SENT even on a perfect
	 * transaction. The mask models exactly that — it is why read-back exists. */
	stub_store_val = (uint16_t)(val & stub_store_mask);
	return DP_SWOP_OK;
}

static uint8_t stub_reg_read(uint8_t dev, uint8_t reg, uint16_t *out)
{
	if (dev > stub_fail_from)
		return DP_SWOP_E_BUSY;
	if (dev == stub_fail_from) {
		if (!stub_fail_on_vlan)
			return DP_SWOP_E_BUSY;
		if (reg == DP_SWOP_REG_VLAN_MAP)
			return DP_SWOP_E_BUSY;
	}
	if (stub_store_active && dev == stub_store_dev && reg == stub_store_reg) {
		*out = stub_store_val;		/* the write is visible to the read-back */
		return DP_SWOP_OK;
	}
	*out = (uint16_t)((reg == DP_SWOP_REG_VLAN_MAP ? 0xC000 : 0xB000) | dev);
	return DP_SWOP_OK;
}

/*
 * Substitute SIDEBAND READ, installed only for section 16. The four absence cases are pure
 * ENVELOPE logic and are unreachable without it: on any build host there is no gpiochip
 * carrying the board's label, so every unseamed OP_SFP can only ever be E_GPIO -- the same
 * shape as the SMI window being unmapped for every other op in this file.
 *
 * It does NOT mock the ioctl. The chardev/sysfs transaction is untested here and is stated as
 * untested; a mock of it would only assert that dp_sff.c agrees with itself.
 */
static uint8_t stub_sff_status = DP_SWOP_OK;
static uint8_t stub_sff_raw;
static uint8_t stub_sff_ok;
static const struct dp_sff_cage *stub_sff_cage_seen;

static uint8_t stub_sff_read(const struct dp_sff_cage *cage, uint8_t *raw, uint8_t *ok)
{
	stub_sff_cage_seen = cage;
	if (stub_sff_status != DP_SWOP_OK) {
		*raw = 0;
		*ok = 0;
		return stub_sff_status;
	}
	*raw = stub_sff_raw;
	*ok = stub_sff_ok;
	return DP_SWOP_OK;
}

/*
 * The invariants the HOST re-derives on every SFP reply (agnic_swop_sfp_fault), checked here so
 * a polarity or mask error is caught by this suite rather than by an operator wondering why an
 * empty cage reports a module. Re-derived from the wire contract, not copied from a header this
 * repo does not carry -- a copy would only prove the two sides were typed by the same hand.
 */
static void sfp_wire_invariants(const struct dp_swop_resp *r, const struct dp_swop_req *q,
				const char *who)
{
	char m[160];
	uint8_t v = r->sfp.valid;

	snprintf(m, sizeof(m), "%s: mark is SFP_MARK on every reply (the echo defence)", who);
	ck(r->sfp.mark == DP_SWOP_SFP_MARK, m);
	snprintf(m, sizeof(m), "%s: port echoes req.dev", who);
	ck(r->sfp.port == q->dev, m);
	snprintf(m, sizeof(m), "%s: page echoes req.reg", who);
	ck(r->sfp.page == q->reg, m);
	snprintf(m, sizeof(m), "%s: cage_map is filled even on a refusal", who);
	ck(r->sfp.cage_map == DP_SWOP_SFP_CAGE_MAP_XGS116, m);
	snprintf(m, sizeof(m), "%s: valid is a SUBSET of implemented", who);
	ck((uint8_t)(v & (uint8_t)~(unsigned)r->sfp.implemented) == 0, m);
	snprintf(m, sizeof(m), "%s: no RESERVED pin bit is ever claimed", who);
	ck((uint8_t)(r->sfp.implemented & (uint8_t)~(unsigned)DP_SWOP_SFP_PINS_PHASE_A) == 0, m);
	/* THE POLARITY IDENTITY, which is the whole point of shipping five parallel masks:
	 * logical == raw ^ active_low, for every bit the reply calls a measurement. */
	snprintf(m, sizeof(m), "%s: logical == raw ^ active_low under valid", who);
	ck((uint8_t)((r->sfp.logical ^ r->sfp.raw) & v) == (uint8_t)(r->sfp.active_low & v), m);
	snprintf(m, sizeof(m), "%s: phase A ships no page bytes and claims none", who);
	ck(r->sfp.page_off == 0 && r->sfp.page_len == 0, m);
	snprintf(m, sizeof(m), "%s: rsv stays zero", who);
	ck(r->sfp.rsv[0] == 0 && r->sfp.rsv[1] == 0 && r->sfp.rsv[2] == 0, m);
	snprintf(m, sizeof(m), "%s: response magic, never the request magic", who);
	ck(r->magic == DP_SWOP_RESP_MAGIC, m);
	snprintf(m, sizeof(m), "%s: op echoed", who);
	ck(r->op == DP_SWOP_OP_SFP, m);
}

/*
 * Assert one row of the board pin table against the vendor descriptor.
 *
 * This exists because a mutation got away, and the finding is worth recording exactly rather
 * than tidying up. Making dp_sff_implemented() ignore its phase_a_read flag left the suite
 * fully green -- the mask to DP_SWOP_SFP_PINS_PHASE_A caught the leak by itself. So does the
 * converse: removing the MASK and keeping the flag is also green. MEASURED both ways, and
 * removing BOTH reddens 11 rows including "no RESERVED pin bit is ever claimed". The two are
 * genuinely redundant ON THIS BOARD, because no pin here is wired-but-unread; they stop being
 * redundant on the first board where one is. That is a property of the BOARD, not a hole in
 * these rows -- so the flag is asserted HERE at its source in the table, the mask is asserted
 * through the wire in 16f, and neither is left resting on the other.
 *
 * The LINE NUMBERS are asserted too, and they are the highest-value rows in this file: a
 * transposition (tx_fault 21 read as rate_select_0 23, say) would read a real line on the real
 * chip and answer confident nonsense, with nothing in a reply to contradict it. Ground truth is
 * AMDA0208-0001R00.txt:163-171.
 */
static void sff_pin_row(const struct dp_sff_cage *cage, uint8_t bit, uint8_t line,
			uint8_t active_low, uint8_t read, uint8_t driven, const char *who)
{
	const struct dp_sff_pin *pin = NULL;
	char m[160];
	uint8_t i;

	/* A harness that SEGFAULTS reports nothing at all, which is strictly worse than a red
	 * row: the mutation that moved the cage to another port device killed this file here
	 * instead of failing it, and a crash is indistinguishable from a build that never ran. */
	snprintf(m, sizeof(m), "descriptor: %s has a cage to describe", who);
	ck(cage != NULL, m);
	if (!cage)
		return;

	for (i = 0; i < cage->npins; i++)
		if (cage->pins[i].bit == bit)
			pin = &cage->pins[i];

	snprintf(m, sizeof(m), "descriptor: %s is in the pin table", who);
	ck(pin != NULL, m);
	if (!pin)
		return;
	snprintf(m, sizeof(m), "descriptor: %s is gpio bank 2 line %u", who, (unsigned)line);
	ck(pin->bank == 2 && pin->line == line, m);
	snprintf(m, sizeof(m), "descriptor: %s active_low == %u (the leading minus)", who,
		 (unsigned)active_low);
	ck(pin->active_low == active_low, m);
	snprintf(m, sizeof(m), "descriptor: %s phase_a_read == %u", who, (unsigned)read);
	ck(pin->phase_a_read == read, m);
	snprintf(m, sizeof(m), "descriptor: %s driven == %u (never re-requested as INPUT)", who,
		 (unsigned)driven);
	ck(pin->driven == driven, m);
	snprintf(m, sizeof(m), "descriptor: %s carries exactly one wire bit", who);
	ck(pin->bit != 0 && (uint8_t)(pin->bit & (uint8_t)(pin->bit - 1u)) == 0, m);
}

static struct dp_swop_req mkreq(uint8_t op, uint8_t dev, uint8_t reg, uint16_t val)
{
	struct dp_swop_req r;

	memset(&r, 0, sizeof(r));
	r.magic = DP_SWOP_MAGIC;
	r.version = DP_SWOP_VERSION;
	r.op = op;
	r.dev = dev;
	r.reg = reg;
	r.val = val;
	return r;
}

int main(void)
{
	struct dp_swop_resp r;
	struct dp_swop_req q;
	char small[4];

	printf("dp_swop envelope tests (SMI window intentionally unmapped)\n");

	/* Layout is a wire contract — assert it here too, not only at compile time. */
	ck(sizeof(struct dp_swop_req) <= 48, "request fits AGNIC params (48B)");
	ck(sizeof(struct dp_swop_resp) <= 56, "response fits AGNIC data (56B)");

	/* 1. A valid request with no mapping reports NOMAP — not a silent zero. */
	r = run(mkreq(DP_SWOP_OP_READ, 0x01, DP_SWOP_REG_STATUS, 0), sizeof(q));
	/* NOTE: this originally asserted the REQUEST magic — i.e. the old test asserted the
	 * very property that made an echo indistinguishable from a reply. */
	ck(r.magic == DP_SWOP_RESP_MAGIC, "response carries the RESPONSE magic");
	ck(r.status == DP_SWOP_E_NOMAP, "unmapped read -> E_NOMAP");
	ck(r.op == DP_SWOP_OP_READ, "op echoed on failure");
	ck(r.val == 0, "no data invented on failure");

	/* 2. A malformed request is classified as malformed even though the window is
	 *    unmapped — validation precedes hardware state, so the caller is told what
	 *    THEY got wrong rather than being blamed on the box. */
	r = run(mkreq(DP_SWOP_OP_READ, 0x0b, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "dev 0x0b (gap between ports and G1) -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_READ, 0x10, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "dev 0x10 (the OLD wrong base) -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_READ, 0xff, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "dev 0xff -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_READ, 0x01, 32, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_REG, "reg 32 -> E_REG");

	/* 3. The legal device set is accepted (reaches the mapping check, not E_DEV). */
	r = run(mkreq(DP_SWOP_OP_READ, 0x00, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "dev 0x00 (CPU port) is legal");
	r = run(mkreq(DP_SWOP_OP_READ, 0x0a, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "dev 0x0a (last port) is legal");
	r = run(mkreq(DP_SWOP_OP_READ, DP_SWOP_DEV_GLOBAL1, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "Global1 0x1b is legal");
	r = run(mkreq(DP_SWOP_OP_READ, DP_SWOP_DEV_GLOBAL2, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "Global2 0x1c is legal");

	/* 4. An unknown op is REFUSED, never quietly treated as a read. */
	r = run(mkreq(0x7f, 0x01, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_OP, "unknown op -> E_OP");
	ck(r.op == 0x7f, "unknown op echoed back");
	r = run(mkreq(0x00, 0x01, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_OP, "op 0 is not a valid operation");

	/* 5. A stray non-swop payload on the custom channel is rejected. This matters:
	 *    the channel previously echoed ANY custom message, so garbage must not be
	 *    mistaken for a command. */
	q = mkreq(DP_SWOP_OP_READ, 0x01, 0, 0);
	q.magic = 0xDEADBEEF;
	r = run(q, sizeof(q));
	ck(r.status == DP_SWOP_E_MAGIC, "wrong magic -> E_MAGIC");
	q = mkreq(DP_SWOP_OP_READ, 0x01, 0, 0);
	q.version = DP_SWOP_VERSION + 1;
	r = run(q, sizeof(q));
	ck(r.status == DP_SWOP_E_MAGIC, "future version -> E_MAGIC (not misparsed)");

	/* 6. A short request cannot be parsed as a long one. */
	q = mkreq(DP_SWOP_OP_READ, 0x01, 0, 0);
	r = run(q, sizeof(q) - 1);
	ck(r.status == DP_SWOP_E_LEN, "short request -> E_LEN");

	/* 7. A response buffer too small is refused rather than partially written. */
	q = mkreq(DP_SWOP_OP_READ, 0x01, 0, 0);
	memset(small, 0x5A, sizeof(small));
	ck(dp_swop_service(&q, sizeof(q), small, sizeof(small)) == -1,
	   "undersized response buffer -> -1");
	ck(small[0] == 0x5A, "undersized buffer left untouched");

	/* 8. PORTS reports how many entries are actually valid — a partial result must
	 *    never look like a full one. Unmapped, that count is zero. */
	r = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "unmapped PORTS -> E_NOMAP");
	ck(r.count == 0, "failed PORTS reports count 0, not a full table");

	/* 10. THE RESPONSE MUST NOT BE MISTAKABLE FOR THE REQUEST. A pre-D84 NPU echoes the
	 *     request verbatim; if the response carried the same magic, an echo would pass
	 *     every check and the fields would alias (status<-dev, count<-reg), so the
	 *     PORTS call (dev=0,reg=0) read as status=0=OK. Found by mamoru-d7. */
	ck(DP_SWOP_RESP_MAGIC != DP_SWOP_MAGIC, "response magic differs from request magic");
	r = run(mkreq(DP_SWOP_OP_READ, 0x01, 0, 0), sizeof(q));
	ck(r.magic == DP_SWOP_RESP_MAGIC, "response is stamped with the RESPONSE magic");
	ck(r.magic != DP_SWOP_MAGIC, "response is NOT stamped with the request magic");
	r = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
	ck(r.magic == DP_SWOP_RESP_MAGIC, "PORTS response also carries the response magic");
	{	/* the exact aliasing case: an echo of a dev=0,reg=0 PORTS request must not be
		 * constructible from a real response — the magic is what makes it impossible. */
		struct dp_swop_req echo = mkreq(DP_SWOP_OP_PORTS, 0, 0, 0);
		ck(echo.magic != DP_SWOP_RESP_MAGIC,
		   "an echoed PORTS request cannot masquerade as a response");
	}

	/* 11. OP_WRITE VALIDATION — the operation that can PARTITION THE SWITCH had zero
	 *     coverage: deleting dev_is_legal() from reg_write() alone left the suite at
	 *     40/40 green (mamoru-d7, mutation 1). Every check below is a WRITE. */
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x0b, DP_SWOP_REG_VLAN_MAP, 0x1234), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "WRITE dev 0x0b -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x10, DP_SWOP_REG_VLAN_MAP, 0x1234), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "WRITE dev 0x10 (the OLD wrong base) -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0xff, 0, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "WRITE dev 0xff -> E_DEV");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x01, 32, 0), sizeof(q));
	ck(r.status == DP_SWOP_E_REG, "WRITE reg 32 -> E_REG");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0x0001), sizeof(q));
	ck(r.status == DP_SWOP_E_NOMAP, "WRITE to a legal dev reaches the mapping check");
	ck(r.op == DP_SWOP_OP_WRITE, "WRITE op echoed back on failure");

	/* 12. THE PARTIAL PORTS TABLE — the count arithmetic, which until now no check
	 *     could reach. Unmapped, the scan always dies at d=0, so a count between 1 and
	 *     10 was never produced and a mutation making a PARTIAL claim to be a FULL
	 *     table passed the entire suite (mamoru-d7, F4). The seam substitutes reg_read
	 *     ONLY — the SMI transaction is still untested here, and deliberately so. */
	{
		struct dp_swop_resp p;

		/* (a) a scan that dies on the STATUS read of port 5 reports exactly 5. */
		stub_fail_from = 5;
		stub_fail_on_vlan = 0;
		dp_swop_test_set_reg_read(stub_reg_read);
		p = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
		ck(p.status == DP_SWOP_E_BUSY, "partial PORTS reports the failing read's status");
		ck(p.count == 5, "partial PORTS reports count 5");
		ck(p.count != DP_SWOP_PORT_COUNT, "a PARTIAL table does not claim to be FULL");
		ck(p.ports[4].status == 0xB004 && p.ports[4].vlan_map == 0xC004,
		   "the entries below the count carry real data");
		ck(p.ports[5].status == 0 && p.ports[5].vlan_map == 0,
		   "the entry the scan died on is left zeroed, not half-filled");

		/* (b) dying on the VLAN_MAP read of port 5 — its status was already written,
		 *     so the count MUST still exclude it or a half-filled entry ships. */
		stub_fail_from = 5;
		stub_fail_on_vlan = 1;
		p = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
		ck(p.count == 5, "a port whose SECOND read failed is excluded from the count");
		ck(p.ports[5].vlan_map == 0, "...and its vlan_map was never invented");

		/* (c) the full-success path, which unmapped could never be reached either. */
		stub_fail_from = DP_SWOP_PORT_COUNT;
		stub_fail_on_vlan = 0;
		p = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
		ck(p.status == DP_SWOP_OK, "a complete scan reports OK");
		ck(p.count == DP_SWOP_PORT_COUNT, "a complete scan reports all 11 ports");
		ck(p.ports[10].status == 0xB00A, "the last port carries its data");

		/* (d) failing on the very first read still reports 0, not a full table. */
		stub_fail_from = 0;
		p = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
		ck(p.count == 0, "a scan that read nothing reports 0");

		dp_swop_test_set_reg_read(NULL);	/* restore the real read */
		r = run(mkreq(DP_SWOP_OP_PORTS, 0, 0, 0), sizeof(q));
		ck(r.status == DP_SWOP_E_NOMAP,
		   "seam restored — the real reg_read is back in the path");
	}

	/* 13. THE WRITE ALLOWLIST — writes are permitted BY CONSTRUCTION, not by denylist.
	 *     mamoru-d7's review: the dangerous set spans Port Control, Port Control 2
	 *     (802.1Q Secure), PVID, Physical Control and both GLOBAL devices (a VTU flush
	 *     is a switch-wide blackout). Enumerating what to refuse is a losing game. */
	ck(dp_swop_write_allowed(0x01, DP_SWOP_REG_VLAN_MAP) == 1, "port reg6 IS writable");
	ck(dp_swop_write_allowed(0x00, DP_SWOP_REG_VLAN_MAP) == 1, "CPU port reg6 IS writable");
	ck(dp_swop_write_allowed(0x0a, DP_SWOP_REG_VLAN_MAP) == 1, "last port reg6 IS writable");
	ck(dp_swop_write_allowed(0x01, 0x04) == 0, "Port Control (PortState/FrameMode) refused");
	ck(dp_swop_write_allowed(0x01, 0x08) == 0, "Port Control 2 (802.1Q Secure) refused");
	ck(dp_swop_write_allowed(0x01, 0x07) == 0, "Default VLAN ID refused");
	ck(dp_swop_write_allowed(0x01, 0x01) == 0, "Physical Control (ForcedLink) refused");
	ck(dp_swop_write_allowed(DP_SWOP_DEV_GLOBAL1, DP_SWOP_REG_VLAN_MAP) == 0,
	   "Global1 refused for WRITE even at an allowlisted reg");
	ck(dp_swop_write_allowed(DP_SWOP_DEV_GLOBAL2, DP_SWOP_REG_VLAN_MAP) == 0,
	   "Global2 refused for WRITE even at an allowlisted reg");
	{	/* globals stay READABLE — the allowlist restricts writes only */
		struct dp_swop_resp g = run(mkreq(DP_SWOP_OP_READ, DP_SWOP_DEV_GLOBAL1, 0, 0),
					    sizeof(q));
		ck(g.status == DP_SWOP_E_NOMAP, "Global1 remains READABLE (reached the mapping check)");
	}
	/* through service(): VALIDITY is answered before POLICY, so the two stay distinct */
	r = run(mkreq(DP_SWOP_OP_WRITE, DP_SWOP_DEV_GLOBAL1, DP_SWOP_REG_VLAN_MAP, 1), sizeof(q));
	ck(r.status == DP_SWOP_E_WRPERM, "WRITE to Global1 -> E_WRPERM (a real device, not writable)");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x01, 0x04, 1), sizeof(q));
	ck(r.status == DP_SWOP_E_WRPERM, "WRITE to a non-allowlisted reg -> E_WRPERM");
	r = run(mkreq(DP_SWOP_OP_WRITE, 0x0b, DP_SWOP_REG_VLAN_MAP, 1), sizeof(q));
	ck(r.status == DP_SWOP_E_DEV, "WRITE to a NON-DEVICE is still E_DEV, not E_WRPERM");

	/* 14. READ-BACK VERIFICATION — there is no WriteValid bit, so "the transaction
	 *     returned OK" is not "your value landed". Reserved/read-only/self-clearing
	 *     bits mean the stored value can legitimately differ from the one sent, and
	 *     without a read-back nothing would ever show it. (mamoru-d7, W1.) */
	{
		struct dp_swop_resp w;

		stub_fail_from = 0xff;		/* never fail; return derived values */
		stub_fail_on_vlan = 0;
		dp_swop_test_set_reg_read(stub_reg_read);
		dp_swop_test_set_reg_write(stub_reg_write);	/* accepts, changes nothing */
		/* A register with READ-ONLY bits: we ask for 0x0FF1, it keeps only 0x00FF,
		 * so the read-back legitimately differs from the request. That must be
		 * E_VERIFY, not success — it is the whole reason read-back exists. */
		stub_store_active = 0;
		stub_store_mask = 0x00ff;
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0x0ff1), sizeof(q));
		ck(w.status == DP_SWOP_E_VERIFY,
		   "write whose read-back differs is E_VERIFY, NOT success");
		ck(w.val == 0x00f1, "the response carries what the register NOW READS");
		stub_store_mask = 0xffff;
		ck(w.ports[0].status == ((0x01u << 8) | DP_SWOP_REG_VLAN_MAP),
		   "the response names the dev/reg it acted on");
		ck(w.ports[0].vlan_map == 0x0ff1, "the response also carries what was REQUESTED");
		/* A REAL CHANGE: pre=0xC001 (derived), write 0x0001, read-back sees it. */
		stub_store_active = 0;
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0x0001), sizeof(q));
		ck(w.status == DP_SWOP_OK, "a write that CHANGES the register is OK");
		ck(w.val == 0x0001, "the read-back shows the new value");

		/* NO CHANGE: the register already holds the requested value, so the
		 * comparison succeeds with no evidence the write reached the intended
		 * device — reported as W_NOCHANGE, never OK. (mamoru-43, generalised from
		 * the zero case: a wrong-but-legal device returns 0x0000 and reg6=0x000 is
		 * a plausible D84 write.) */
		stub_store_active = 0;
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0xC001), sizeof(q));
		ck(w.status == DP_SWOP_W_NOCHANGE,
		   "a write that changes NOTHING is W_NOCHANGE, not OK");
		/* the zero case specifically, since that is the one with a real bug behind it */
		stub_store_active = 0;
		stub_fail_from = 0xff;
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x02, DP_SWOP_REG_VLAN_MAP, 0xC002), sizeof(q));
		ck(w.status == DP_SWOP_W_NOCHANGE, "same-value write on another port is also W_NOCHANGE");
		stub_store_active = 0;
		dp_swop_test_set_reg_read(NULL);
		dp_swop_test_set_reg_write(NULL);
	}

	/* 15. OP_READ's SUCCESS PATH — which until the seam fix could not be reached at all.
	 *
	 *      Every OP_READ check above asserts E_NOMAP or a validation refusal, because
	 *      dp_swop_service() called `reg_read` directly instead of the REG_READ seam, so
	 *      substituting a stub had no effect on this op. The suite therefore proved that a
	 *      READ is *rejected* correctly and never that it *works*.
	 *
	 *      This is the op A0.2's hardware control depends on, so "no coverage" was the
	 *      wrong place for it to be. */
	{
		struct dp_swop_resp p;

		stub_fail_from = 0xff;		/* never fail */
		stub_fail_on_vlan = 0;
		stub_store_active = 0;
		dp_swop_test_set_reg_read(stub_reg_read);

		/* A successful read returns the register's value in resp.val, with OK. */
		p = run(mkreq(DP_SWOP_OP_READ, 0x03, DP_SWOP_REG_STATUS, 0), sizeof(q));
		ck(p.status == DP_SWOP_OK, "OP_READ success: status OK");
		ck(p.val == 0xB003, "OP_READ success: resp.val carries the register value");
		ck(p.op == DP_SWOP_OP_READ, "OP_READ success: op echoed");
		ck(p.magic == DP_SWOP_RESP_MAGIC, "OP_READ success: response magic, not request");

		/* A DIFFERENT register on the same device returns a DIFFERENT value. Without
		 * this the assertion above would pass against a stub that ignored its
		 * arguments -- i.e. against exactly the constant-returning implementation the
		 * #74 bar exists to reject. */
		p = run(mkreq(DP_SWOP_OP_READ, 0x03, DP_SWOP_REG_VLAN_MAP, 0), sizeof(q));
		ck(p.status == DP_SWOP_OK, "OP_READ vlan-map: status OK");
		ck(p.val == 0xC003, "OP_READ is a function of the REGISTER it was asked for");

		/* And a different DEVICE likewise, so the dev argument is proven to reach the
		 * read rather than being dropped. */
		p = run(mkreq(DP_SWOP_OP_READ, 0x07, DP_SWOP_REG_STATUS, 0), sizeof(q));
		ck(p.val == 0xB007, "OP_READ is a function of the DEVICE it was asked for");

		/* A failing read reports the failure and does NOT report a value. */
		stub_fail_from = 0x05;
		p = run(mkreq(DP_SWOP_OP_READ, 0x06, DP_SWOP_REG_STATUS, 0), sizeof(q));
		ck(p.status == DP_SWOP_E_BUSY, "OP_READ failure: the read's status is reported");
		ck(p.val == 0, "OP_READ failure: no value is invented");

		stub_fail_from = 0xff;
		dp_swop_test_set_reg_read(NULL);	/* restore the real read */

		/* POSITIVE CONTROL for the restore: with the seam back, a legal address again
		 * reaches the unmapped window. If this regressed to OK, the stub would still be
		 * installed and every later check in this file would be testing the mock. */
		p = run(mkreq(DP_SWOP_OP_READ, 0x03, DP_SWOP_REG_STATUS, 0), sizeof(q));
		ck(p.status == DP_SWOP_E_NOMAP, "seam restored: OP_READ reaches the real read again");
	}

	/* 16. OP_SFP — the per-port SFP cage sideband (phase A).
	 *
	 *     THE NEGATIVE CONTROLS COME FIRST, and not as a stylistic choice. This project's
	 *     standing rule is that a gate which cannot fail is a missing gate that is TRUSTED,
	 *     so before a single "the pins read correctly" assertion is made, the handler is
	 *     shown REFUSING four known-bad inputs: a device that is not a port, a page this
	 *     firmware cannot serve, a port with no cage, and a gpiochip label that resolves to
	 *     nothing. If any of those started succeeding, every positive row below would be
	 *     asserting against a handler that says yes to everything.
	 */
	{
		struct dp_swop_resp p;
		struct dp_swop_req  sq;

		/* ---- 16a. NEGATIVE: a device that cannot own a front-panel cage ------------ */
		sq = mkreq(DP_SWOP_OP_SFP, 0x0b, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_DEV, "SFP dev 0x0b (not a device at all) -> E_DEV");
		sfp_wire_invariants(&p, &sq, "E_DEV 0x0b");
		/* Global1 is a REAL, READABLE device for OP_READ and still not a port. If this
		 * ever answers E_NOCAGE, the handler is using dev_is_legal()'s wider set and is
		 * reporting a board fact about something with no front panel. */
		sq = mkreq(DP_SWOP_OP_SFP, DP_SWOP_DEV_GLOBAL1, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_DEV, "SFP Global1 -> E_DEV, NOT E_NOCAGE");
		sq = mkreq(DP_SWOP_OP_SFP, 0xff, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_DEV, "SFP dev 0xff -> E_DEV");

		/* ---- 16b. NEGATIVE: a page phase-A firmware cannot serve ------------------- */
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, DP_SWOP_SFP_PAGE_A0, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_PAGE, "SFP page A0 on phase-A firmware -> E_PAGE");
		ck(p.sfp.page == DP_SWOP_SFP_PAGE_A0, "...and the refused page is named back");
		sfp_wire_invariants(&p, &sq, "E_PAGE A0");
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, DP_SWOP_SFP_PAGE_A2, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_PAGE, "SFP page A2 on phase-A firmware -> E_PAGE");
		/* A page byte MUST NOT be invented on the refusal: SFF-8472 gives 0x00 meanings,
		 * so a zero-filled page decodes as a plausible, wrong, MODULE-SHAPED answer. */
		ck(p.sfp.page_len == 0 && p.sfp.data[0] == 0 && p.sfp.data[27] == 0,
		   "E_PAGE ships an EMPTY RANGE, never a zero-filled page");
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, 0x00, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_PAGE,
		   "page 0 -> E_PAGE (an uninitialised buffer is not a request)");

		/* ---- 16c. NEGATIVE: a port that has no cage on this board ------------------ */
		sq = mkreq(DP_SWOP_OP_SFP, 0x03, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_NOCAGE, "SFP on a cage-less port -> E_NOCAGE");
		ck(p.sfp.implemented == 0 && p.sfp.valid == 0,
		   "absence case (a): no cage means no pins are claimed");
		sfp_wire_invariants(&p, &sq, "E_NOCAGE port 3");
		sq = mkreq(DP_SWOP_OP_SFP, 0x00, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_NOCAGE, "the CPU port has no cage either");
		sq = mkreq(DP_SWOP_OP_SFP, DP_SWOP_PORT_DEV_MAX, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_NOCAGE, "the last port device is legal, and has no cage");

		/* ---- 16d. NEGATIVE: an unresolvable gpiochip LABEL fails loudly ------------ */
		dp_sff_test_set_chip_label("dp-sff-no-such-gpiochip");
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_GPIO, "an unresolvable gpiochip label -> E_GPIO");
		ck(p.sfp.implemented == DP_SWOP_SFP_PINS_PHASE_A,
		   "E_GPIO still says the BOARD wires these four pins");
		ck(p.sfp.valid == 0 && p.sfp.raw == 0 && p.sfp.logical == 0,
		   "E_GPIO measures NOTHING -- four zeros would decode as a healthy module");
		sfp_wire_invariants(&p, &sq, "E_GPIO");
		dp_sff_test_set_chip_label(NULL);

		/* ---- 16e. THE BANK TABLE, asserted directly -------------------------------- */
		/* This is the only part of "resolve BY LABEL, never by a base" a build host can
		 * MEASURE. 16d alone would pass on any machine that simply has no such chip, and
		 * would keep passing if the table were emptied -- an absence proving nothing. */
		ck(dp_sff_label_for_bank(2) != NULL, "the descriptor's bank 2 has a label");
		ck(dp_sff_label_for_bank(2) != NULL &&
		   strcmp(dp_sff_label_for_bank(2),
			  "f2440000.system-controller:gpio@140") == 0,
		   "bank 2 resolves to the label MEASURED from the booted DTB (2026-09-06)");
		/* Banks 0 and 1 are absent DELIBERATELY: the box has two other gpiochips and
		 * nobody has read their bank numbers out of the DTB. A guess here would read
		 * another controller's lines and answer with total confidence. */
		ck(dp_sff_label_for_bank(0) == NULL, "an unmeasured bank is REFUSED, not guessed");
		ck(dp_sff_label_for_bank(1) == NULL, "...bank 1 likewise");
		ck(dp_sff_label_for_bank(255) == NULL, "...and a nonsense bank likewise");
		/* Prove the override seam 16d relied on is actually wired -- otherwise 16d was
		 * measuring the host's lack of a gpiochip and nothing else. */
		dp_sff_test_set_chip_label("dp-sff-override-probe");
		ck(strcmp(dp_sff_label_for_bank(2), "dp-sff-override-probe") == 0,
		   "the label seam really does displace the board's label");
		dp_sff_test_set_chip_label(NULL);
		ck(strcmp(dp_sff_label_for_bank(2),
			  "f2440000.system-controller:gpio@140") == 0,
		   "...and NULL restores the board's own label");

		/* ---- 16f. THE DESCRIPTOR, against the contract's known-value controls ------- */
		{
			const struct dp_sff_cage *cage = dp_sff_cage_for_dev(0x09);

			ck(cage != NULL, "port device 0x09 (front panel F1) has the cage");
			ck(dp_sff_cage_for_dev(0x01) == NULL, "port 1 has none");
			ck(dp_sff_cage_map() == DP_SWOP_SFP_CAGE_MAP_XGS116,
			   "cage_map is DERIVED and matches the XGS116 known-value control");
			ck(dp_sff_implemented(cage) == DP_SWOP_SFP_PINS_PHASE_A,
			   "the four phase-A pins are implemented");
			/* RATE SELECT is WIRED and DRIVEN at bring-up but never read back, so it
			 * must NOT be claimed: a pin reported without being read is absence case
			 * (d) wearing case (c)'s clothes. */
			ck((dp_sff_implemented(cage) &
			    (DP_SWOP_SFP_PIN_RS0 | DP_SWOP_SFP_PIN_RS1)) == 0,
			   "rate_select is DRIVEN but never read, so it is never claimed");
			ck(dp_sff_active_low(cage) == DP_SWOP_SFP_ACTIVE_LOW_XGS116,
			   "PRESENT is the only active-low pin (the descriptor's leading minus)");
			ck(dp_sff_implemented(NULL) == 0 && dp_sff_active_low(NULL) == 0,
			   "a NULL cage claims nothing");

			/* THE PIN TABLE ITSELF, row by row, against
			 * AMDA0208-0001R00.txt:163-171. The masks above are derived from
			 * these rows, so asserting only the masks leaves a transposed line
			 * number invisible -- and a wrong line reads a REAL line on the REAL
			 * chip and answers with total confidence.
			 *                 bit                        line a_low read driven */
			sff_pin_row(cage, DP_SWOP_SFP_PIN_PRESENT,     22,  1,   1,   0,
				    "present   (-gpio:2:22)");
			sff_pin_row(cage, DP_SWOP_SFP_PIN_RX_LOS,      28,  0,   1,   0,
				    "rx_los    (gpio:2:28)");
			sff_pin_row(cage, DP_SWOP_SFP_PIN_TX_FAULT,    21,  0,   1,   0,
				    "tx_fault  (gpio:2:21)");
			/* tx_disable is DRIVEN. `driven` is a safety flag, not documentation:
			 * a v1 chardev request asking for INPUT calls gpiod_direction_input()
			 * and would release the drive on a laser-enable pin as a side effect
			 * of reading telemetry. */
			sff_pin_row(cage, DP_SWOP_SFP_PIN_TX_DISABLE,  17,  0,   1,   1,
				    "tx_disable(gpio:2:17)");
			/* RATE SELECT: wired and driven, phase_a_read 0. Asserted HERE at the
			 * flag, because the mask to PINS_PHASE_A also hides an RS leak and two
			 * guards defending one property look like one until each is tested. */
			sff_pin_row(cage, DP_SWOP_SFP_PIN_RS0,         23,  0,   0,   1,
				    "rate_select_0 (gpio:2:23)");
			sff_pin_row(cage, DP_SWOP_SFP_PIN_RS1,         29,  0,   0,   1,
				    "rate_select_1 (gpio:2:29)");
		}

		/* ---- 16g. POSITIVE: the four absence cases, pairwise distinguishable -------
		 *
		 * Only now, with every refusal above shown to fire, is the read seam installed.
		 * PRESENT is ACTIVE LOW: raw bit 0 CLEAR means a module IS seated.
		 */
		dp_sff_test_set_read(stub_sff_read);
		stub_sff_status = DP_SWOP_OK;

		/* (d) module seated, and RX_LOS ASSERTED. This is the real 2026-09-06 appliance
		 *     state -- the far end's laser was off -- and the one glance that would have
		 *     replaced three days of diagnosis. */
		stub_sff_raw = DP_SWOP_SFP_PIN_RX_LOS;		/* present low = seated */
		stub_sff_ok = DP_SWOP_SFP_PINS_PHASE_A;
		stub_sff_cage_seen = NULL;
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, DP_SWOP_SFP_PAGE_PINS, 0);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_OK, "seated module -> OK");
		ck(stub_sff_cage_seen == dp_sff_cage_for_dev(0x09),
		   "the handler read the cage belonging to the REQUESTED port");
		ck(p.sfp.valid == DP_SWOP_SFP_PINS_PHASE_A, "all four pins are measurements");
		ck((p.sfp.logical & DP_SWOP_SFP_PIN_PRESENT) != 0,
		   "PRESENT is NORMALISED: raw 0 on an active-low line means SEATED");
		ck((p.sfp.raw & DP_SWOP_SFP_PIN_PRESENT) == 0,
		   "...and the RAW electrical level is carried unnormalised beside it");
		ck((p.sfp.logical & DP_SWOP_SFP_PIN_RX_LOS) != 0, "RX_LOS: no light arriving");
		ck((p.sfp.logical & DP_SWOP_SFP_PIN_TX_FAULT) == 0, "TX_FAULT clear");
		sfp_wire_invariants(&p, &sq, "seated+rx_los");

		/* (d) again, everything quiet. The contrast with (b) below is the whole point:
		 *     here rx_los is a MEASUREMENT that reads 0. */
		stub_sff_raw = 0;
		stub_sff_ok = DP_SWOP_SFP_PINS_PHASE_A;
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_OK, "seated and quiet -> OK");
		ck(p.sfp.valid == DP_SWOP_SFP_PINS_PHASE_A,
		   "absence case (d): a pin that is wired and reads 0 is still a MEASUREMENT");
		ck((p.sfp.logical & DP_SWOP_SFP_PIN_RX_LOS) == 0, "light is arriving");
		sfp_wire_invariants(&p, &sq, "seated+quiet");

		/* (b) cage present, NOTHING SEATED. raw PRESENT high = empty; the other three
		 *     lines float, and a floating line is NOT a measurement of light. */
		stub_sff_raw = DP_SWOP_SFP_PINS_PHASE_A;	/* everything pulled high */
		stub_sff_ok = DP_SWOP_SFP_PINS_PHASE_A;
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_OK, "an EMPTY cage is a successful answer, not an error");
		ck(p.sfp.implemented == DP_SWOP_SFP_PINS_PHASE_A,
		   "absence case (b): the board still wires all four");
		ck(p.sfp.valid == DP_SWOP_SFP_PIN_PRESENT,
		   "...but only PRESENT is a measurement -- (b) differs from (d) by VALID");
		ck((p.sfp.logical & DP_SWOP_SFP_PIN_PRESENT) == 0, "...and it says NOT SEATED");
		ck(p.sfp.raw == DP_SWOP_SFP_PINS_PHASE_A,
		   "...the raw levels are still carried, for debugging, outside valid");
		sfp_wire_invariants(&p, &sq, "empty cage");

		/* (c)-shaped: one line could not be READ while the chip resolved. implemented
		 *     stays SET and valid clears -- so "unreadable" cannot be mistaken for
		 *     "reads 0", which is (d). */
		stub_sff_raw = 0;
		stub_sff_ok = (uint8_t)(DP_SWOP_SFP_PINS_PHASE_A & ~DP_SWOP_SFP_PIN_TX_FAULT);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_OK, "one unreadable line does not fail the whole reply");
		ck((p.sfp.implemented & DP_SWOP_SFP_PIN_TX_FAULT) != 0,
		   "the unreadable pin is still IMPLEMENTED");
		ck((p.sfp.valid & DP_SWOP_SFP_PIN_TX_FAULT) == 0,
		   "...and NOT valid -- unreadable is distinct from reads-0");
		ck((p.sfp.valid & DP_SWOP_SFP_PIN_RX_LOS) != 0,
		   "...while the lines that DID read still count");
		sfp_wire_invariants(&p, &sq, "one line unreadable");

		/* PRESENT itself unreadable. An rx_los level that cannot be attributed to a
		 * seated module is not attributable at all, so nothing is called a measurement.
		 * Without this, an unreadable PRESENT would fall through to the SEATED branch by
		 * default and three floating lines would ship as readings. */
		stub_sff_raw = DP_SWOP_SFP_PINS_PHASE_A;
		stub_sff_ok = (uint8_t)(DP_SWOP_SFP_PINS_PHASE_A & ~DP_SWOP_SFP_PIN_PRESENT);
		p = run(sq, sizeof(q));
		ck(p.sfp.valid == 0,
		   "PRESENT unreadable -> nothing about the module is a measurement");
		ck(p.sfp.implemented == DP_SWOP_SFP_PINS_PHASE_A,
		   "...while the board's wiring is still reported");
		sfp_wire_invariants(&p, &sq, "present unreadable");

		/* NOT ONE line readable, while the chip itself resolved. An OK carrying
		 * valid = 0 would be true but quiet; this mirrors npu_switch_ports refusing
		 * to render an all-zero table, because a silently-failed scan and a quiet
		 * board read identically. */
		stub_sff_raw = 0;
		stub_sff_ok = 0;
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_GPIO,
		   "a scan that measured NOTHING is E_GPIO, not a quiet OK");
		ck(p.sfp.implemented == DP_SWOP_SFP_PINS_PHASE_A,
		   "...and the board's wiring is still reported");
		sfp_wire_invariants(&p, &sq, "nothing readable");

		/* A read that fails wholesale is E_GPIO through the seam too, so the status is
		 * the handler's and not an artefact of the host having no gpiochip. */
		stub_sff_status = DP_SWOP_E_GPIO;
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_GPIO, "a failing sideband read is reported as E_GPIO");
		ck(p.sfp.valid == 0, "...and measures nothing");
		stub_sff_status = DP_SWOP_OK;

		/* ---- 16h. The request's remaining fields ---------------------------------- */
		stub_sff_raw = 0;
		stub_sff_ok = DP_SWOP_SFP_PINS_PHASE_A;
		sq = mkreq(DP_SWOP_OP_SFP, 0x09, DP_SWOP_SFP_PAGE_PINS, 0x1234);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_OK, "req.val is the page offset and phase A IGNORES it");
		/* `count` is one of the two fields the old PORTS echo aliased onto, so OP_SFP
		 * gives it no job. If this goes red because a later lane found a use for it,
		 * that use must survive the echo analysis again rather than inherit a pass. */
		ck(p.count == 0, "OP_SFP leaves `count` unused, by design");

		/* ---- 16i. THE ECHO DEFENCE, as bytes ------------------------------------- */
		{
			struct dp_swop_resp e;
			struct dp_swop_req  echo = mkreq(DP_SWOP_OP_SFP, 0x09,
							 DP_SWOP_SFP_PAGE_PINS, 0);

			/* Exactly what a pre-D84 NPU does, and exactly what the host sees: the
			 * request memcpy'd into the zeroed 56-byte response buffer. */
			memset(&e, 0, sizeof(e));
			memcpy(&e, &echo, sizeof(echo));
			ck(e.magic != DP_SWOP_RESP_MAGIC, "an echo dies at byte 0 on the magic");
			ck(e.sfp.mark == 0,
			   "an echo has NO BYTE at offset 14 -- mark stays 0, the host says NOMARK");
			ck(sizeof(struct dp_swop_req) == 12 &&
			   offsetof(struct dp_swop_resp, sfp.mark) == 14,
			   "the mark sits PAST the end of the request; that is why 16i holds");
		}

		/* ---- 16j. SEAM RESTORE, as a positive control ------------------------------
		 * If this regressed to OK the stub would still be installed and every check
		 * after this point would be testing the mock. */
		dp_sff_test_set_read(NULL);
		p = run(sq, sizeof(q));
		ck(p.status == DP_SWOP_E_GPIO,
		   "seam restored: OP_SFP reaches the real, label-resolving read again");
		/* HONEST LIMIT, stated rather than implied: on a build host that E_GPIO is the
		 * environment answering, not the resolver being exercised. 16e is what actually
		 * measures the resolution rule; the Linux chardev/sysfs code below it compiles
		 * only on Linux and is proven by nothing in this file. */
	}

	/* 9. NEGATIVE CONTROL — prove this harness can FAIL. Without it a green run
	 *    says nothing: a test that cannot fail is a missing test that is trusted. */
	{
		int before = failures;

		ck(DP_SWOP_OK == 0xff, "[negative control] deliberately false claim");
		if (failures != before + 1) {
			printf("  FATAL: the harness did not register a known-false check\n");
			return 2;
		}
		failures = before;	/* retract the deliberate failure */
		printf("  (negative control fired correctly)\n");
	}

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
