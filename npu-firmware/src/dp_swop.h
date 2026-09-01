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
 * WRITE HAZARD — read this before wiring a caller.
 * A write to a Port-Based VLAN Map (reg 6) can partition the switch. The management
 * path rides a front port, so a map that isolates that port cuts the box off the
 * network, and recovery is then a MAINS cycle. This module deliberately does NOT
 * encode which port is "management" — the NPU does not know that, and a mechanism
 * that guesses would be a policy in disguise. Refusing to partition management is
 * the CALLER's obligation, and it belongs in the applier where the interface map is
 * actually known.
 */

#ifndef DP_SWOP_H
#define DP_SWOP_H

#include <stdint.h>

#define DP_SWOP_MAGIC		0x53574F50u	/* "SWOP" — rejects a stray custom msg  */
#define DP_SWOP_VERSION		1u

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
	uint32_t	magic;		/* echoed DP_SWOP_MAGIC              */
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
_Static_assert(sizeof(struct dp_swop_req) <= 48, "swop request exceeds AGNIC params");
_Static_assert(sizeof(struct dp_swop_resp) <= 56, "swop response exceeds AGNIC data");
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
 * Bounded and non-blocking: every SMI wait is a bounded spin, so this cannot stall
 * the management pump thread it runs on. */
int dp_swop_service(const void *req, unsigned req_len, void *resp, unsigned resp_cap);

#endif /* DP_SWOP_H */
