/* SPDX-License-Identifier: MIT
 *
 * dp_swop.h — D84 switch-access operations over the AGNIC custom local-flow channel.
 *
 * The host (x86) sends a `struct dp_swop_req` as the payload of an
 * NMP_GUEST_LF_T_CUSTOM message; the NPU answers with a `struct dp_swop_resp` in the
 * same buffer. This is the transport-independent half: it knows nothing about NMP and
 * can be unit-tested against a mock register file.
 *
 * Wire budget (fixed by the AGNIC ABI, agnic_abi.h):
 *   request  payload  AGNIC_MGMT_PARAMS_LEN     = 48 bytes
 *   response payload  AGNIC_MGMT_DESC_DATA_LEN  = 56 bytes
 * Both structures below are sized to fit with room to spare, and are explicitly
 * padded + static_asserted so a compiler on either side cannot silently disagree.
 *
 * ADDRESSING — see swmdio.sh. Port devices are 0x00..0x0a (device address == port
 * number, multi-chip SMI-addr-2 indirect protocol), Global1 = 0x1b, Global2 = 0x1c.
 * A device address outside that set is REFUSED rather than issued: a wrong address
 * returns 0x0000 with the SMI VALID bit SET, so the bus cannot tell you that you
 * asked the wrong question (verified on hardware 2026-09-01).
 *
 * WRITES ARE ALLOWLISTED — see dp_swop_write_allowed(). This is the load-bearing
 * decision of the write path, and it INVERTS the obvious design. A denylist of
 * dangerous registers is a losing game: 13 devices x 32 registers, with a dangerous
 * set that is large, family-specific and partly MODE-dependent. Beyond the VLAN Map
 * named below it includes at least Port Control (PortState=Disabled stops forwarding;
 * FrameMode on the CPU port breaks the host tagging contract), Port Control 2 (802.1Q
 * Secure with no matching VTU entry drops ALL of a port's traffic — a mode change, not
 * a visibly topological one), Default VLAN ID, Physical Control (ForcedLink can force
 * the management link down), and on the GLOBAL devices a VTU flush, which under Secure
 * mode is a switch-wide blackout rather than one port's isolation. (mamoru-d7's review.)
 *
 * So only the (dev,reg) pairs D84 needs are permitted; everything else is refused BY
 * CONSTRUCTION rather than by having been thought of. An unforeseen dangerous register
 * is refused because it was never allowed — the property a denylist cannot have.
 * WRITES TO GLOBAL1/GLOBAL2 ARE REFUSED ENTIRELY; they remain readable.
 *
 * EVERY WRITE IS READ BACK. The bus offers no WriteValid bit, so "the transaction
 * returned OK" is not "your value landed". Switch registers also carry reserved,
 * read-only and self-clearing bits, so the value that LANDS can legitimately differ
 * from the value SENT even on a perfect transaction. The response therefore carries
 * the post-write register contents plus the dev/reg it acted on, and a mismatch is
 * reported as DP_SWOP_E_VERIFY rather than success.
 *
 * WRITE HAZARD — read this before wiring a caller.
 * A write to a Port-Based VLAN Map (reg 6) can partition the switch. The management
 * path rides a front port, so a map that isolates that port cuts the box off the
 * network, and recovery is then a MAINS cycle. This module deliberately does NOT
 * encode which port is "management" — the NPU does not know that, and a mechanism
 * that guesses would be a policy in disguise. Refusing to partition management is
 * the CALLER's obligation, and it belongs in the applier where the interface map is
 * actually known.
 *
 * TWO LIMITS ON THAT OBLIGATION, so a caller-side guard is not written under a false
 * sense of completeness (both from d7's review):
 *  (a) NOTHING HERE TELLS THE CALLER WHICH PORT IS MANAGEMENT. PORTS returns
 *      status+vlanmap per device with no indication of the CPU port or the operator's
 *      link, so the obligation rests on board knowledge outside this contract — and a
 *      guard that does not know which port is management cannot refuse to partition it.
 *  (b) A PARTITION CAN BE BUILT FROM INDIVIDUALLY-SAFE WRITES. Removing the management
 *      port from every OTHER port's map isolates it without any single write touching
 *      its own registers. No per-write guard can see that at any allowlist quality; the
 *      check must be against the RESULTING map, before the first write of a sequence.
 */

#ifndef DP_SWOP_H
#define DP_SWOP_H

#include <stddef.h>
#include <stdint.h>

#define DP_SWOP_MAGIC		0x53574F50u	/* "SWOP" — request  */
#define DP_SWOP_RESP_MAGIC	0x53574F52u	/* "SWOR" — response */
#define DP_SWOP_VERSION		1u

/* THE RESPONSE MAGIC MUST DIFFER FROM THE REQUEST MAGIC. This is not cosmetic.
 * A pre-D84 NPU echoes the request back VERBATIM, so a response that shared the
 * request magic is byte-indistinguishable from an echo — and the fields then ALIAS:
 * resp.status lands on req.dev and resp.count on req.reg. For the PORTS call the host
 * actually makes (dev=0, reg=0) that yields status=0=OK, i.e. an echo from firmware
 * WITHOUT the handler was reported as SUCCESS, and the "firmware predates D84" branch
 * was unreachable code. Measured over all 29 legal device addresses by mamoru-d7:
 * detector fired 0 times. Distinct magics make echo detection TOTAL and independent
 * of every other field. */

/* Operations. Unknown op => DP_SWOP_E_OP, never a fallback to something plausible. */
#define DP_SWOP_OP_READ		0x01u	/* one register                              */
#define DP_SWOP_OP_WRITE	0x02u	/* one register                              */
#define DP_SWOP_OP_PORTS	0x03u	/* bulk: status+vlanmap for every port dev   */

/* Status codes. 0 is the ONLY success value. */
#define DP_SWOP_OK		0x00u
#define DP_SWOP_E_MAGIC		0x01u	/* payload was not a swop request            */
#define DP_SWOP_E_OP		0x02u	/* unknown operation                         */
#define DP_SWOP_E_DEV		0x03u	/* device address outside the legal set      */
#define DP_SWOP_E_REG		0x04u	/* register outside 0..31                    */
#define DP_SWOP_E_BUSY		0x05u	/* SMI stayed busy — bounded spin expired    */
#define DP_SWOP_E_INVALID	0x06u	/* transaction completed but ReadValid clear */
#define DP_SWOP_E_NOMAP		0x07u	/* /dev/mem window not mapped (init failed)  */
#define DP_SWOP_E_LEN		0x08u	/* caller passed a short buffer              */
#define DP_SWOP_E_WRPERM	0x09u	/* (dev,reg) not on the WRITE allowlist      */
#define DP_SWOP_E_VERIFY	0x0au	/* write landed but read-back != requested   */

#define DP_SWOP_PORT_DEV_MAX	0x0au	/* port devices are 0x00..0x0a               */
#define DP_SWOP_DEV_GLOBAL1	0x1bu
#define DP_SWOP_DEV_GLOBAL2	0x1cu
#define DP_SWOP_PORT_COUNT	(DP_SWOP_PORT_DEV_MAX + 1u)	/* 11 */

/* Registers this module names. Others are still readable by number. */
#define DP_SWOP_REG_STATUS	0x00u	/* Port Status: link(11) duplex(10) speed(9:8) */
#define DP_SWOP_REG_SWITCH_ID	0x03u	/* reads 0x1930 at ANY device — never a probe  */
#define DP_SWOP_REG_VLAN_MAP	0x06u	/* Port-Based VLAN Map — what D84 writes       */

struct dp_swop_req {
	uint32_t	magic;		/* DP_SWOP_MAGIC                     */
	uint8_t		version;	/* DP_SWOP_VERSION                   */
	uint8_t		op;		/* DP_SWOP_OP_*                      */
	uint8_t		dev;		/* device address (validated)        */
	uint8_t		reg;		/* register 0..31 (validated)        */
	uint16_t	val;		/* WRITE only; ignored otherwise     */
	uint16_t	rsv;		/* must be 0                         */
};					/* 12 bytes                          */

struct dp_swop_port {
	uint16_t	status;		/* reg 0 */
	uint16_t	vlan_map;	/* reg 6 */
};					/* 4 bytes */

struct dp_swop_resp {
	uint32_t	magic;		/* DP_SWOP_RESP_MAGIC, never the
					 * request magic — see above         */
	uint8_t		version;
	uint8_t		op;		/* echoed, so a reply cannot be
					 * mistaken for a different one      */
	uint8_t		status;		/* DP_SWOP_OK or an error            */
	uint8_t		count;		/* PORTS: entries valid in ports[]   */
	uint16_t	val;		/* READ result                       */
	uint16_t	rsv;
	struct dp_swop_port ports[DP_SWOP_PORT_COUNT];	/* PORTS only, 44 B  */
};					/* 12 + 44 = 56 bytes exactly        */

/* Fail the BUILD, not the wire, if either side's layout drifts. */
#ifdef __GNUC__
/* EXACT, not <=. A capacity assert pins nothing: adding a field to dp_swop_req moved
 * sizeof 12->16 with both capacity asserts still passing and the host's exact
 * BUILD_BUG_ON in the other repo never seeing it — silent cross-repo layout drift,
 * the precise hazard the duplication is supposed to be guarded against. */
_Static_assert(sizeof(struct dp_swop_req) == 12, "swop request layout drifted");
_Static_assert(sizeof(struct dp_swop_resp) == 56, "swop response layout drifted");
/* And sizes agreeing is NOT layouts agreeing — pin the offsets that carry meaning. */
_Static_assert(offsetof(struct dp_swop_req, op) == 5, "req.op moved");
_Static_assert(offsetof(struct dp_swop_req, dev) == 6, "req.dev moved");
_Static_assert(offsetof(struct dp_swop_req, reg) == 7, "req.reg moved");
_Static_assert(offsetof(struct dp_swop_req, val) == 8, "req.val moved");
_Static_assert(offsetof(struct dp_swop_resp, status) == 6, "resp.status moved");
_Static_assert(offsetof(struct dp_swop_resp, count) == 7, "resp.count moved");
_Static_assert(offsetof(struct dp_swop_resp, val) == 8, "resp.val moved");
_Static_assert(offsetof(struct dp_swop_resp, ports) == 12, "resp.ports moved");
#endif

/* Map the SMI window. Call ONCE at startup. Returns 0, or -errno.
 * Safe to call when already mapped (idempotent). */
int dp_swop_init(void);

/* Release the mapping (test teardown; dp_fwd never needs it). */
void dp_swop_fini(void);

/* Service one request. `req_len`/`resp_cap` are the caller's actual buffer sizes.
 * ALWAYS writes a well-formed response — an error is reported IN the response, so a
 * failure can never be mistaken for a message that was never handled. Returns the
 * number of response bytes written, or -1 if resp_cap was too small for even a header.
 *
 * Bounded and non-blocking: ONE transaction spends at most SPIN_BUDGET MMIO accesses
 * in total, across every nested wait — a single shared budget, not per-loop limits that
 * would multiply. That is what makes this safe on the management pump thread, where an
 * unbounded wait starves the host handshake and tears the link down. */
int dp_swop_service(const void *req, unsigned req_len, void *resp, unsigned resp_cap);

/* Is (dev,reg) permitted for WRITE? Exposed so a test can drive the policy directly
 * rather than inferring it from service() outcomes. */
int dp_swop_write_allowed(uint8_t dev, uint8_t reg);

#ifdef DP_SWOP_TEST
/* Substitute the per-register read so the PORTS COUNT arithmetic is observable — the
 * only part of the envelope no test could reach, because every check runs with the
 * window unmapped and the scan therefore always dies at d=0. Pass NULL to restore.
 *
 * This does NOT mock the SMI transaction (hardware-proven; a mock there would only
 * assert this file agrees with itself). It exists solely so a PARTIAL table can be
 * produced and its count checked. Absent DP_SWOP_TEST there is no pointer and no hook:
 * dp_fwd calls reg_read directly. */
void dp_swop_test_set_reg_read(uint8_t (*fn)(uint8_t dev, uint8_t reg, uint16_t *out));
void dp_swop_test_set_reg_write(uint8_t (*fn)(uint8_t dev, uint8_t reg, uint16_t val));
#endif

#endif /* DP_SWOP_H */
