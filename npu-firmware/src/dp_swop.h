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
 * A WRITE THAT CHANGES NOTHING PROVES NOTHING — DP_SWOP_W_NOCHANGE.
 * The read-back compares the post-write value against the requested one. That test is
 * blind in exactly one case: when the register ALREADY held the requested value, the
 * comparison succeeds without any evidence the write reached the intended device.
 * mamoru-43 found this for the zero case, which is the likely one — the 2026-09-01
 * base-address bug returned valid=1 with data=0x0000 from a WRONG device, and
 * `reg6 = 0x000` (isolate a port completely) is a plausible D84 operation, so a write
 * of zero to a wrong-but-legal device reads back zero and looks correct. The general
 * form is broader than zero: ANY value that the addressed register already holds is
 * indistinguishable from a write that landed elsewhere on a register holding the same.
 * And the obvious prober cannot help — reg 3 reads 0x1930 at EVERY device address, so
 * it cannot discriminate one device from another.
 *
 * So the handler pre-reads, and reports W_NOCHANGE rather than OK when the value did
 * not move. That is not a failure; it is a refusal to claim evidence that does not
 * exist. A caller that genuinely wants to write a value already present can accept it.
 *
 * EVERY WRITE IS READ BACK. The bus offers no WriteValid bit, so "the transaction
 * returned OK" is not "your value landed". Switch registers also carry reserved,
 * read-only and self-clearing bits, so the value that LANDS can legitimately differ
 * from the value SENT even on a perfect transaction. The response therefore carries
 * the post-write register contents plus the dev/reg it acted on, and a mismatch is
 * reported as DP_SWOP_E_VERIFY rather than success.
 *
 * WRITE HAZARD — PRECONDITION, not a note. reg 6 HAS AN INCIDENT.
 * On 2026-08-08 a port brought up before its VLAN map was written egressed to every
 * other front port and BRIDGED A CUSTOMER'S NETWORKS INSIDE THE SWITCH, looping a live
 * LAN. Not hypothetical: port 9 was later found holding reg6=0x05ff on the live box
 * while every other front port held 0x001 (HARDWARE.md, sw-init.sh).
 *
 * The bring-up path's protection is ORDERING — maps written before phyup. **That
 * protection is structurally unavailable to D84**, which writes reg 6 on ports that are
 * ALREADY UP: every D84 write happens in exactly the state the ordering rule exists to
 * avoid. The allowlist guards the ADDRESS; this incident is about the VALUE, and
 * `0x7fe` on a front port is a perfectly well-formed reg-6 write that bridges every
 * front port. (mamoru-43's second read.)
 *
 * THEREFORE: a caller MUST validate the RESULTING map — the whole port vector set it
 * intends to leave behind — BEFORE issuing the first write of a sequence. Per-write
 * validation cannot see this, in either direction: a bridge assembled from
 * individually-well-formed values, or a partition assembled from individually-safe
 * ones (d7's W5). This is a precondition on the caller, not advice.
 *
 * A write to a Port-Based VLAN Map (reg 6) can also partition the switch. The management
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
#define DP_SWOP_OP_SFP		0x04u	/* per-port SFP cage sideband — see below    */
/* 0x04 was confirmed FREE on BOTH sides before it was claimed: the opcode namespace held only
 * 0x01/0x02/0x03 here and in the host's agnic_swop.h. (0x04 is also E_REG, but that is the
 * STATUS namespace; the two never share a field.) */

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
#define DP_SWOP_W_NOCHANGE	0x0bu	/* read-back agrees, but NOTHING CHANGED —
					 * the write is UNVERIFIABLE, see below      */

/* 0x0c..0x0e belong to OP_SFP. They are non-zero — status != OK — because the ABSENCE CONTRACT
 * this opcode inherits forbids answering an unanswerable question with a zero: SFF-8472 gives
 * 0x00 a meaning ("rate not specified"), so a zero from a bus that was never there decodes as a
 * plausible, wrong, MODULE-SHAPED answer. Each of these says WHICH question could not be
 * answered instead.
 *
 * E_NOCAGE IS AN ANSWER, NOT A FAULT. "port 3 has no SFP cage" is a true, useful board fact; it
 * is carried as a status only because status is the field that cannot be mistaken for a reading.
 * The handler MUST still fill port/page/mark/cage_map on this path — see struct dp_swop_sfp. */
#define DP_SWOP_E_NOCAGE	0x0cu	/* the addressed port device has no SFP cage on this
					 * board — distinct from an EMPTY cage       */
#define DP_SWOP_E_PAGE		0x0du	/* unknown/unsupported page selector. THIS is what
					 * phase-A firmware answers to a phase-B A0/A2
					 * request — never a zero-filled page, which
					 * would decode as a real module             */
#define DP_SWOP_E_GPIO		0x0eu	/* the cage's GPIO controller could not be resolved
					 * BY LABEL, or a line read failed outright.
					 * NEVER a guessed chip base: Linux GPIO bases
					 * are not stable across kernels and a wrong
					 * base reads OTHER pins and answers with
					 * confidence. On this board the descriptor's
					 * `gpio2` is the chip labelled
					 * "f2440000.system-controller:gpio@140"
					 * (measured 2026-09-06: Linux gpiochip64 on
					 * that boot, ngpio 31 — the LABEL is the key,
					 * the base is not). A single line that fails
					 * clears its `valid` bit instead.           */

/* THE CEILING, and it did not exist on this side until OP_SFP added to it.
 *
 * The host has carried AGNIC_SWOP_STATUS_MAX from the beginning and its drift test pins that
 * every value up to it has a NAME. This side had no twin — which is precisely how 0x09..0x0b
 * were added here and went unnamed on the host for weeks: the repo that GROWS the vocabulary was
 * the one with nothing declaring how far it had grown. A ceiling only the consumer holds is a
 * ceiling the producer can walk past without noticing.
 *
 * BUMP THIS IN THE SAME COMMIT as any new status code, and bump the host's twin in the same
 * breath. scripts/swop-contract-diff.sh in the host repo compares the two by value. */
#define DP_SWOP_STATUS_MAX	0x0eu

#define DP_SWOP_PORT_DEV_MAX	0x0au	/* port devices are 0x00..0x0a               */
#define DP_SWOP_DEV_GLOBAL1	0x1bu
#define DP_SWOP_DEV_GLOBAL2	0x1cu
#define DP_SWOP_PORT_COUNT	(DP_SWOP_PORT_DEV_MAX + 1u)	/* 11 */

/* Registers this module names. Others are still readable by number. */
#define DP_SWOP_REG_STATUS	0x00u	/* Port Status: link(11) duplex(10) speed(9:8) */
#define DP_SWOP_REG_SWITCH_ID	0x03u	/* reads 0x1930 at ANY device — never a probe  */
#define DP_SWOP_REG_VLAN_MAP	0x06u	/* Port-Based VLAN Map — what D84 writes       */

/* ---- OP_SFP: the per-port SFP cage sideband (phase A) --------------------------------------
 *
 * WHAT IT IS FOR, in one sentence: on 2026-09-06 the appliance's only optical port was dark for
 * three days and the answer — the far end's laser was off (Nami CS110 lan25: TX -40 dBm, 0.002 mA
 * bias, hardware TX_DISABLE asserted) — was two bytes of sideband this board already had wired
 * and nothing in our software ever read. `rx_los` says "no light is arriving" in one glance.
 *
 * PER PORT, NOT PER BOARD. On the XGS 116 exactly one device has a cage — 0x09, front panel F1,
 * vendor `npu0.eth8` — and it would have been a byte cheaper to define this opcode as "tell me
 * about THE cage". That is a board fact leaking into a wire format. Other boards in this family
 * carry several cages and the fabric direction adds Nami switches with four, so the request names
 * a PORT DEVICE (0x00..0x0a, the same address space every other swop op uses) and the reply
 * describes THAT cage. `cage_map` below tells a caller which iterations are worth making, in one
 * round trip, so the generality costs no extra traffic.
 *
 * THE REQUEST reuses `struct dp_swop_req` unchanged — no new request type, no layout churn:
 *   op   = DP_SWOP_OP_SFP
 *   dev  = the port device whose cage is being asked about, 0x00..0x0a. Outside that set the
 *          handler answers DP_SWOP_E_DEV, exactly as READ and WRITE do; OP_SFP invents no second
 *          addressing style.
 *   reg  = the PAGE SELECTOR (DP_SWOP_SFP_PAGE_*). Phase A sends PAGE_PINS.
 *   val  = byte offset within that page. Phase A sends 0 and the handler ignores it.
 *
 * HOW PHASE B FITS WITHOUT A FIFTH OPCODE OR A WIRE BREAK — requirement, not aspiration.
 * The A0 (SFF-8431 identity, i2c 0x50) and A2 (SFF-8472 diagnostics, i2c 0x51) pages are 128
 * bytes each and the whole response payload is 44, so a page cannot be one reply however it is
 * laid out. The selector + offset already in the request, and page_off/page_len/data[] already in
 * the reply, make phase B a matter of this handler answering PAGE_A0/PAGE_A2 instead of refusing
 * them with DP_SWOP_E_PAGE. Nothing on the wire moves and no constant changes value.
 *
 * AND THE PARSE THAT COST THIS PROJECT WEEKS IS NOT REPEATED HERE. The descriptor says
 * `npu0.phy8.SFF-8431=i2c1:2:0x50`, and the vendor library parses it with the literal format
 * "i2c1:%i:%i" — `i2c1` is a FIXED PREFIX TOKEN and the FIRST number is the BUS. The cage is
 * /dev/i2c-2 address 0x50, WITH NO MUX (measured 2026-09-06: i2c-0 and i2c-2 exist, i2c-1 is
 * `status="disabled"` in the DTB, both 0x50 and 0x51 answer on bus 2, and there is no mux at
 * 0x70-0x77). This contract carries NO bus or address field ON PURPOSE: the descriptor lives on
 * THIS side and so does its parsing, and a bus number on the wire is one more place to re-make
 * that mistake.
 *
 * WHAT THE HANDLER MUST FILL, AND WHICH FIELD IS WHICH. Written down because a normalisation
 * whose direction is not written down is how a polarity bug survives review:
 *
 *   raw          the ELECTRICAL level read at the GPIO line, before any inversion.
 *   active_low   which bits THIS BOARD's descriptor marks active low. `present` is `-gpio:2:22`
 *                — the leading minus IS the polarity — so on the XGS 116 this is exactly
 *                DP_SWOP_SFP_ACTIVE_LOW_XGS116. Sent because the descriptor lives here; the host
 *                must never hold board polarity of its own.
 *   logical      the NORMALISED, ACTIVE-HIGH value: 1 means the condition named by the bit is
 *                TRUE. logical == raw ^ active_low, for every bit, and the host RE-DERIVES that
 *                identity on every read (agnic_swop_sfp_fault). Getting it wrong is caught by
 *                the consumer, not by an operator wondering why an empty cage reports a module.
 *   implemented  the BOARD wires this pin. Static, from the descriptor.
 *   valid        THIS REPLY's `logical` bit is a measurement. MUST be a subset of implemented.
 *
 * THE FOUR ABSENCE CASES, ALL DISTINGUISHABLE, NONE OF THEM A BARE ZERO — the same rule
 * sfp-rate-read.sh pre-registered for the EEPROM and dp_swop's own PORTS table follows:
 *
 *   (a) this port has no cage at all       status = E_NOCAGE; implemented = 0, valid = 0; still
 *                                          fill port/page/mark/cage_map.
 *   (b) cage present, nothing seated       status = OK; implemented = the wired set (0x0f on this
 *                                          board); valid = PIN_PRESENT ONLY; logical's PRESENT
 *                                          bit = 0. DO NOT set valid for rx_los/tx_fault/
 *                                          tx_disable here — a floating line is not a
 *                                          measurement of light, and reporting it as a zero
 *                                          reading is the manufactured-zero this opcode exists
 *                                          to avoid.
 *   (c) pin not wired on this board        implemented bit CLEAR (and therefore valid clear).
 *   (d) pin wired, and it reads 0          implemented SET, valid SET, logical bit 0.
 *
 * GPIO RESOLUTION IS BY LABEL, AND IT FAILS LOUDLY. Measured 2026-09-06: the booted DTB maps the
 * descriptor's `gpio2` to /cp0/config-space/system-controller@440000/gpio@140, status="okay",
 * ngpios 31 — i.e. the chip whose sysfs label is "f2440000.system-controller:gpio@140", which was
 * Linux gpiochip64 ON THAT BOOT. **Do not hardcode base 64.** Linux GPIO bases are not stable
 * across kernels, nothing is exported, there are no gpio-line-names and debugfs is not mounted,
 * so a guessed base reads OTHER pins and answers with total confidence. Resolve the chip by
 * label; if the label is absent, answer DP_SWOP_E_GPIO. Never fall back.
 *
 * THE ECHO DEFENCE — the single most important property of this layout.
 *
 * A pre-D84 NPU echoes the request back VERBATIM. When the magics were the same, that echo passed
 * every host check and the fields ALIASED: resp.status landed on req.dev and resp.count on
 * req.reg, so the PORTS call (dev=0, reg=0) read back as status=0=OK and firmware WITHOUT the
 * handler was reported as SUCCESS. OP_SFP is answerable by an echo in NEITHER of the two ways
 * that bug needed, and the reasons are structural rather than lucky:
 *
 *  1. The magics still differ ("SWOR" vs "SWOP"), so a verbatim echo is refused at byte 0.
 *  2. THE ALIAS IS DESIGNED OUT. Every field the host's verdict depends on — `port`, `page`,
 *     `mark` — lives at offset 12 or beyond, and `struct dp_swop_req` is TWELVE BYTES. An echo
 *     physically cannot supply them: there is no request byte at those offsets to echo. The host
 *     zeroes its 56-byte response buffer before the send, so an echoing peer leaves 12..55 as
 *     ZERO, and `mark` is required to be DP_SWOP_SFP_MARK, a fixed non-zero constant. The verdict
 *     is ECHO or NOMARK, deterministically, never OK.
 *  3. The two fields the old bug aliased ONTO are deliberately NOT load-bearing: `count` is
 *     consulted by nothing in the host's SFP verdict, and `status` only AFTER `mark` and `port`
 *     have attributed the reply — so even dev=0, exactly what aliased to status=0=OK for PORTS,
 *     cannot reach a success verdict.
 *  4. And the send is gated on DP_SWOP_CAP_SFP, so firmware predating this opcode is never sent
 *     one. That matters beyond correctness: an unanswered custom send strands the management
 *     channel and the only recovery is a mains cycle (DEBT #80). The capability gate is the
 *     primary defence; 1-3 are what stands if it is ever wrong.
 */
#define DP_SWOP_SFP_MARK	0x5346u	/* "SF". Fixed, NON-ZERO, at reply offset 14 — past the
					 * end of the 12-byte request, so no echo can produce
					 * it. Fill it on EVERY OP_SFP reply, errors included. */

/* Page selectors, carried in req.reg. PAGE_PINS is 0x01 and NOT 0x00 on purpose: a zero selector
 * would make "the default request" and "an uninitialised buffer" the same bytes. 0xa0/0xa2 are
 * the classic 8-bit forms of i2c addresses 0x50/0x51, so the selector names the page an SFF
 * datasheet names rather than inventing a third numbering. */
#define DP_SWOP_SFP_PAGE_PINS	0x01u	/* phase A: sideband pins only              */
#define DP_SWOP_SFP_PAGE_A0	0xa0u	/* phase B: SFF-8431 identity   (i2c 0x50)  */
#define DP_SWOP_SFP_PAGE_A2	0xa2u	/* phase B: SFF-8472 diagnostics (i2c 0x51) */
#define DP_SWOP_SFP_PAGE_LEN	0x80u	/* 128 bytes; both pages, per SFF-8472      */
#define DP_SWOP_SFP_CHUNK	0x1cu	/* 28 bytes of page per reply — what is left of the
					 * 44-byte payload after the fixed fields. A page is
					 * 5 replies; page_off/page_len say which bytes
					 * arrived, so a caller never assumes a stride.     */

/* Sideband pin bits. One numbering shared by implemented/valid/logical/raw/active_low, so a mask
 * can be compared against a value with no translation step — a constant mirrored into a second
 * convention is how a copied constant goes wrong silently. */
#define DP_SWOP_SFP_PIN_PRESENT		0x01u	/* module seated. ACTIVE LOW on the wire */
#define DP_SWOP_SFP_PIN_RX_LOS		0x02u	/* 1 = loss of received light            */
#define DP_SWOP_SFP_PIN_TX_FAULT	0x04u	/* 1 = transmitter fault                 */
#define DP_SWOP_SFP_PIN_TX_DISABLE	0x08u	/* 1 = laser commanded off               */
/* Reserved bit positions for the two RATE SELECT lines. Phase A DRIVES rate_select_0/1 during
 * bring-up (gpio:2:23 and gpio:2:29) but does not READ them back, so phase-A firmware MUST leave
 * these clear in `implemented` — claiming a pin is implemented while reporting a value nobody
 * read is case (d) wearing case (c)'s clothes, which is the exact confusion this contract exists
 * to prevent. Numbered here so adding the readback later is a firmware change, not a wire one. */
#define DP_SWOP_SFP_PIN_RS0		0x10u
#define DP_SWOP_SFP_PIN_RS1		0x20u
#define DP_SWOP_SFP_PINS_PHASE_A	0x0fu	/* the four bits phase A may ever set */

/* KNOWN-VALUE POSITIVE CONTROLS for the XGS 116, in the spirit of DP_SWOP_REG_SWITCH_ID: a
 * plausible reply can be checked against them instead of trusted. They are NOT the source of
 * truth and NOTHING MAY BRANCH ON THEM — this handler derives cage_map and active_low from the
 * board descriptor, because a value hardcoded to this board is wrong on the very next one.
 * Ground truth, AMDA0208-0001R00.txt:163-171: one cage, on device 0x09; `present` is
 * `-gpio:2:22`, the only pin carrying the leading minus. */
#define DP_SWOP_SFP_CAGE_MAP_XGS116	0x0200u	/* bit 9 only == PortF1 */
#define DP_SWOP_SFP_ACTIVE_LOW_XGS116	0x01u	/* PRESENT only         */

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

/*
 * The OP_SFP payload. Occupies the SAME 44 bytes as ports[] — the response is 56 bytes and it is
 * FULL, fixed by the AGNIC ABI's AGNIC_MGMT_DESC_DATA_LEN, so there is nowhere else for it to
 * live and growing the struct is not on the table.
 *
 * EVERY FIELD HERE IS AT OFFSET >= 12, and that is the echo defence, not a coincidence of
 * layout. See the OP_SFP block above, point 2.
 */
struct dp_swop_sfp {
	uint8_t		port;		/* +12 echo of req.dev — the cage this reply
					 *     describes. The anti-alias check: a
					 *     12-byte request has no byte here.     */
	uint8_t		page;		/* +13 echo of req.reg — DP_SWOP_SFP_PAGE_* */
	uint16_t	mark;		/* +14 DP_SWOP_SFP_MARK, always, INCLUDING
					 *     on E_NOCAGE and E_PAGE               */
	uint16_t	cage_map;	/* +16 bit N set => port device N has a cage
					 *     on this board. Sent on EVERY reply so
					 *     one round trip tells a caller which
					 *     ports are worth asking about.        */
	uint8_t		implemented;	/* +18 DP_SWOP_SFP_PIN_* the BOARD wires    */
	uint8_t		valid;		/* +19 ...of which THIS reply measured. MUST
					 *     be a subset of implemented; the host
					 *     rejects a reply where it is not.     */
	uint8_t		logical;	/* +20 NORMALISED, active-high: 1 == the
					 *     named condition is TRUE.
					 *     logical == raw ^ active_low          */
	uint8_t		raw;		/* +21 RAW level, before normalisation      */
	uint8_t		active_low;	/* +22 which bits the descriptor inverts    */
	uint8_t		page_off;	/* +23 phase B: byte offset of data[] within
					 *     the page. 0 in phase A. A page is
					 *     128 B so a byte suffices; a wider page
					 *     is a shape change and bumps VERSION.  */
	uint8_t		page_len;	/* +24 phase B: bytes valid in data[]. 0 in
					 *     phase A — honest, because these two
					 *     describe a RANGE, and an empty range
					 *     is not a zero READING.               */
	uint8_t		rsv[3];		/* +25 must be 0; phase-B flags land here   */
	uint8_t		data[DP_SWOP_SFP_CHUNK];	/* +28..+55 page bytes; 0 in phase A */
};					/* 44 bytes                                 */

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
	/*
	 * ANONYMOUS on purpose. `resp.ports[i]` and `resp.sfp.logical` both stay valid, so
	 * adding OP_SFP changes NO existing consumer in either repo — a named arm would have
	 * renamed `ports` at every call site in dp_swop.c, agnic_main.c and agnic_mgmt.c, and
	 * both trees are shared with concurrent work.
	 *
	 * sizeof stays 56 and offsetof(ports) stays 12, so every assert below and every
	 * BUILD_BUG_ON in the host's agnic_mgmt.c holds unchanged.
	 *
	 * `op` says which arm is live. Reading the wrong arm is reading another op's bytes.
	 */
	union {
		struct dp_swop_port ports[DP_SWOP_PORT_COUNT];	/* PORTS, WRITE echo, 44 B */
		struct dp_swop_sfp  sfp;			/* SFP,                44 B */
	};
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
/* THE SFP ARM. Its offsets ARE the echo defence — every one of them is >= 12, i.e. past the end
 * of the 12-byte request, so no echo can supply them. Pinning them here means that property is
 * enforced by the BUILD rather than re-argued in review each time a field is added. */
_Static_assert(sizeof(struct dp_swop_sfp) == 44, "sfp payload layout drifted");
_Static_assert(sizeof(struct dp_swop_sfp) == sizeof(((struct dp_swop_resp *)0)->ports),
	       "the sfp arm must not grow the response past 56 bytes");
_Static_assert(DP_SWOP_SFP_CHUNK == 28, "chunk size drifted from the 44-byte payload budget");
_Static_assert(offsetof(struct dp_swop_resp, sfp) == 12, "resp.sfp moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.port) == 12, "sfp.port moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.page) == 13, "sfp.page moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.mark) == 14, "sfp.mark moved — echo defence");
_Static_assert(offsetof(struct dp_swop_resp, sfp.cage_map) == 16, "sfp.cage_map moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.implemented) == 18, "sfp.implemented moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.valid) == 19, "sfp.valid moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.logical) == 20, "sfp.logical moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.raw) == 21, "sfp.raw moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.active_low) == 22, "sfp.active_low moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.page_off) == 23, "sfp.page_off moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.page_len) == 24, "sfp.page_len moved");
_Static_assert(offsetof(struct dp_swop_resp, sfp.data) == 28, "sfp.data moved");
#endif

/* Map the SMI window. Call ONCE at startup. Returns 0, or -errno.
 * Safe to call when already mapped (idempotent). */
/* ---- D84 / #24-f: the SWOP CAPABILITY CARRIER --------------------------------------------
 *
 * The host must decide whether this NPU firmware speaks swop BEFORE it is allowed to send one,
 * and every carrier tried before this named the wrong subject. The barmap version belongs to
 * Marvell, so a vendor ABI bump would have opened our gate — which is why the host currently
 * hardcodes the capability to false and telemetry is refused on a production boot.
 *
 * These words are published by OUR firmware into the NW_AGENT window of the host-visible BAR0
 * and read passively by OUR driver. Neither half is vendor-owned, so a vendor change cannot
 * forge them, and no message is sent to discover them — which matters because an unanswered
 * custom send is the thing that strands the management channel (DEBT #80).
 *
 * WHY NW_AGENT. It is the facility the Sophos NetAgent used, and dp_fwd replaced NetAgent, so
 * nothing should write it. The only reference to it anywhere in the MUSDK tree is our own
 * portmap.h. "Should" is confirmed by reading the window on hardware before this publishes into
 * it, not by this comment.
 *
 * THE VERSION IS MATCHED EXACTLY, NEVER `>=`. Firmware and driver ship independently — the NPU
 * rootfs survives host OTAs — so a `>=` test would let a FUTURE firmware that changes the
 * message shape open the gate for TODAY'S driver. Bump it on any shape change.
 *
 * ---- VERSION 2 (2026-09-06, owner ruling) --------------------------------------------------
 *
 * v1 published TWO words. v2 publishes THREE: the tagged feature word documented below. The bump
 * is NOT because appending broke a v1 reader — it did not, words 0 and 1 keep their offsets,
 * types and meanings — it is because THE VERSION IS THE ONLY WORD AN OLD FIRMWARE REWRITES, and
 * that makes it the only thing capable of retiring a stale word a NEW firmware left behind.
 *
 * THE HAZARD IT CLOSES is a designed operation, not a thought experiment. BAR0 memory outlives a
 * dp_fwd restart and is cleared only by a MAINS POWER CYCLE, and A/B OTA makes a firmware
 * DOWNGRADE routine. Sequence: a v2 dp_fwd publishes magic + version + feature word; the box is
 * rolled back; a v1 dp_fwd — which knows nothing of word 2 — republishes ONLY magic and version.
 * Word 2 survives, still carrying the REAL tag 0x5343 and CAP_SFP, because WE wrote it, so the
 * self-tag below cannot see anything wrong with it. A host matching only magic + version would
 * read a genuine-looking grant, send OP_SFP to firmware with no handler, and one unanswered
 * custom send STRANDS THE MANAGEMENT CHANNEL — recovery is a mains cycle (DEBT #80; it happened
 * on 2026-09-02). With the bump, the rolled-back v1 firmware republishes version 1, the host's
 * EXACT match refuses the whole carrier, and the stale feature word goes down with it. The
 * closure is total rather than probabilistic: the old binary rewrites the discriminator itself,
 * so no value any newer build placed in that window can outlive it.
 *
 * THE ACCEPTED COST — owner-ruled, to be DOCUMENTED rather than mitigated, and stated here
 * because this is where a reader meets the constant. The mismatch is SYMMETRIC: an OLD host
 * speaking version 1, against NEW firmware publishing version 2, now fails the same exact match
 * and loses the WHOLE CARRIER — not merely the SFP feature. The host's `npu_swop_capable` goes
 * false, so OP_PORTS is refused with it and the switch port table and `link_up` telemetry go
 * ABSENT on that box until the host catches up. That is a TELEMETRY REGRESSION, not a wedge:
 * nothing is sent, nothing hangs, no mains cycle is needed, and an operator can still re-arm the
 * host's diagnostic path with its `diag` parameter. It is deliberately the safe direction —
 * losing telemetry until the two halves match costs an operator a reading; granting a capability
 * to firmware that cannot answer costs a truck roll.
 *
 * ORDERING IS THE ATOMICITY — A REQUIREMENT ON THE PUBLISHER, written as a requirement and not as
 * an observation of any particular publisher. It must: CLEAR the magic, barrier, write version
 * and features, barrier, write the magic LAST. Clearing FIRST is what makes the magic a commit
 * point on EVERY publish rather than only the first one after a mains cycle — BAR0 memory
 * survives a dp_fwd restart, so on a restart of a box that has published before, the magic is
 * ALREADY LIVE while the words beside it are being rewritten.
 *
 * WHAT ORDERING DOES **NOT** BUY, stated so nothing here outruns the code: it says nothing about
 * a publisher that has DIED. The carrier outlives the process that wrote it — a dp_fwd that
 * publishes and then exits leaves magic, version and features standing with no handler behind
 * them, and the host cannot tell that from a healthy box. Ordering makes the three words mutually
 * consistent; it cannot make them CURRENT. The version match and the self-tag are the only
 * defences, and neither detects a dead process.
 */
#define DP_SWOP_CAP_WINDOW_OFF	0x4000u		/* NW_AGENT base within the host-visible BAR0 */
/* Offset of the slot inside that window (v1: two words; v2: three). CONFIRM AGAINST A HARDWARE
 * SURVEY before the first publishing build ships: the host's `npu_capability` attribute lists
 * every non-zero word in the window, and this slot must land where nothing else writes. */
#define DP_SWOP_CAP_SLOT_OFF	0x0000u
#define DP_SWOP_CAP_MAGIC	0x53574f50u	/* "SWOP" */
#define DP_SWOP_CAP_VERSION	2u	/* v2 = 3-word slot; see the ruling above. Host matches EXACTLY. */

/* ---- THE FEATURE WORD (third word of the slot) -------------------------------------------
 *
 * WHY A THIRD WORD AT ALL. magic+version answer "does this firmware speak swop?". They cannot
 * answer "does it speak THIS OPCODE?", and that distinction is the whole point of gating OP_SFP:
 * the host must be able to tell "firmware predates the SFP opcode" from "this port has no cage".
 * Without it the two collapse into one silence, and the way the host would find out is BY
 * SENDING — and one unanswered custom send is what stranded the management channel on 2026-09-02
 * and costs a mains cycle to clear (DEBT #80). The gate has to be readable PASSIVELY.
 *
 * WHY THE VERSION **IS** BUMPED FOR IT — the full argument is in the carrier block above; this
 * is the short form. The APPEND on its own would not need a bump: words 0 and 1 keep their
 * offsets, types and meanings, so a v1 reader reads exactly what it read before. The DOWNGRADE
 * needs it. A rolled-back v1 publisher rewrites only words 0 and 1 and leaves this word standing
 * with a valid tag, so the version is the only word that can retire it. **ALSO BUMP** if any
 * existing word ever changes meaning, or if the slot shrinks or is reordered.
 *
 * WHY THE WORD VALIDATES ITSELF. A v1 firmware publishes 8 bytes and leaves word 2 as whatever
 * the window already held. The 2026-09-03 survey read that window as exactly 2 non-zero words in
 * 16384, i.e. zero — but "nothing else writes NW_AGENT" is a should, not a measurement that binds
 * the future, and a stale non-zero word here would FORGE a capability and buy the wedge this gate
 * exists to avoid. So the feature word carries its own tag in its high half: the host believes
 * bits only when (word >> 16) == DP_SWOP_CAP_FEAT_TAG. Zero fails it, all-ones (a PCIe UR read)
 * fails it, stale traffic fails it with probability 1 - 2^-16.
 *
 * PUBLISH ORDER — the magic is the commit point, and the full requirement (clear the magic FIRST,
 * then version and features, then the magic last) is stated in the carrier block above. The
 * feature word is written on the same side of the barrier as the version, never after the magic.
 * And the publisher's own size refusal must widen from +8 to +DP_SWOP_CAP_SLOT_LEN, or it writes
 * past the end of a window that is big enough for v1 and not for this.
 */
#define DP_SWOP_CAP_FEATURES_OFF	0x0008u	/* within the slot: WINDOW_OFF+SLOT_OFF+8 */
#define DP_SWOP_CAP_SLOT_LEN		0x000cu	/* magic, version, features */
#define DP_SWOP_CAP_FEAT_TAG		0x5343u	/* "SC" in the feature word's high half */
#define DP_SWOP_CAP_SFP			0x00000001u	/* firmware implements DP_SWOP_OP_SFP */

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
