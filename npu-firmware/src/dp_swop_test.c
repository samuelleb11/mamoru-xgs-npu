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
static uint8_t stub_reg_write(uint8_t dev, uint8_t reg, uint16_t val)
{
	(void)dev; (void)reg; (void)val;
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
	*out = (uint16_t)((reg == DP_SWOP_REG_VLAN_MAP ? 0xC000 : 0xB000) | dev);
	return DP_SWOP_OK;
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
		/* the stub returns 0xC00d for a VLAN_MAP read, which will NOT equal what we
		 * ask to write — so a correct implementation must report a MISMATCH. */
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0x0001), sizeof(q));
		ck(w.status == DP_SWOP_E_VERIFY,
		   "write whose read-back differs is E_VERIFY, NOT success");
		ck(w.val == 0xC001, "the response carries what the register NOW READS");
		ck(w.ports[0].status == ((0x01u << 8) | DP_SWOP_REG_VLAN_MAP),
		   "the response names the dev/reg it acted on");
		ck(w.ports[0].vlan_map == 0x0001, "the response also carries what was REQUESTED");
		/* and the agreeing case must pass: ask for exactly what the stub will return */
		w = run(mkreq(DP_SWOP_OP_WRITE, 0x01, DP_SWOP_REG_VLAN_MAP, 0xC001), sizeof(q));
		ck(w.status == DP_SWOP_OK, "write whose read-back AGREES is OK");
		dp_swop_test_set_reg_read(NULL);
		dp_swop_test_set_reg_write(NULL);
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
