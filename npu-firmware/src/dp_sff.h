/* SPDX-License-Identifier: MIT
 *
 * dp_sff.h — the board's SFP cage descriptor, and sideband GPIO access for DP_SWOP_OP_SFP.
 *
 * Split out of dp_swop.c on purpose. dp_swop.c is the SMI/switch-register file; a cage is a
 * BOARD fact reached over a completely different bus, and mixing the two would have made the
 * one file that must stay auditable — this is the project's first PUBLIC clean-room module —
 * about two unrelated pieces of hardware.
 *
 * WHAT LIVES HERE AND WHAT DOES NOT. This file answers three questions and no others:
 *   - which port devices have a cage on this board  (dp_sff_cage_map)
 *   - which sideband pins that cage wires, on which line, with which polarity  (the table)
 *   - what those lines READ right now                (dp_sff_read_pins)
 * It does NOT decide what any of that MEANS on the wire. The absence contract — which pins may
 * be called a measurement, and which of the four absence cases a reply encodes — is the wire
 * contract's, and it is applied in dp_swop.c's OP_SFP handler where the reply is assembled.
 *
 * SINGLE TRANSLATION UNIT. `forwarder.c` #includes `dp_swop.c`, and `dp_swop.c` #includes
 * `dp_sff.c` at its foot, so this pair adds no object file and no link step to dp_fwd — the
 * same arrangement docs/LICENSING.md already describes for dp_swop.c. build_fwd.sh must copy
 * these two files alongside dp_swop.{c,h}; if it does not, the build fails LOUDLY at the
 * #include rather than quietly producing a dp_fwd without the handler.
 */

#ifndef DP_SFF_H
#define DP_SFF_H

#include <stdint.h>

#include "dp_swop.h"	/* DP_SWOP_SFP_PIN_*, DP_SWOP_OK, DP_SWOP_E_GPIO */

/*
 * One sideband line, as the vendor board descriptor states it. Ground truth for the XGS 116 is
 * platform/sophos-xgs116/bsp/opt/sophos/plt/AMDA0208-0001R00.txt:163-171, e.g.
 *
 *     npu0.phy8.pin.rx_los=gpio:2:28
 *     npu0.phy8.pin.present=-gpio:2:22        <-- the LEADING MINUS is the polarity
 *
 * A TABLE, not inline constants, because the descriptor is per-board and per-port: other boards
 * in this family carry several cages on different lines, and the fabric direction adds Nami
 * switches with four. The wire contract is already per-port for the same reason; hardcoding one
 * cage here would put the board fact back one layer down instead of removing it.
 */
struct dp_sff_pin {
	uint8_t	bit;		/* DP_SWOP_SFP_PIN_* — exactly one bit, in the wire's numbering,
				 * so implemented/valid/logical/raw/active_low all share one
				 * convention and no bit needs translating between layers. */
	uint8_t	bank;		/* the descriptor's `gpio:<bank>:<line>` bank. Resolved to a Linux
				 * gpiochip BY LABEL — see dp_sff.c. NEVER by a base. */
	uint8_t	line;		/* the line WITHIN that bank (the descriptor's second number) */
	uint8_t	active_low;	/* 1 == the descriptor's leading minus. `logical == raw ^ active_low` */
	uint8_t	phase_a_read;	/* 1 == phase A reads this line back. See the note on `implemented`
				 * in dp_sff_implemented(): a pin nobody reads must NOT be claimed. */
	uint8_t	driven;		/* 1 == bring-up DRIVES this line (sfp-init.sh). Load-bearing for
				 * safety, not documentation: a v1 chardev line request that asks
				 * for INPUT would switch the line's DIRECTION, and doing that to
				 * tx_disable de-drives a laser-enable pin. dp_sff.c never asks for
				 * INPUT on a driven line; it clears the pin's `valid` bit instead. */
};

struct dp_sff_cage {
	uint8_t				dev;	/* the switch port device that owns this cage
						 * (0x00..DP_SWOP_PORT_DEV_MAX) */
	const struct dp_sff_pin	       *pins;
	uint8_t				npins;
	/* THE MODULE'S OWN i2c PAGES. This field pair was reserved for "phase B"; the A2
	 * CROSS-CHECK brought it forward, because phase A's problem turned out to need it.
	 *
	 * It is parsed from the vendor descriptor's literal "i2c1:%i:%i", where `i2c1` is a
	 * FIXED PREFIX TOKEN and the FIRST number is the BUS. `npu0.phy8.SFF-8431=i2c1:2:0x50`
	 * is BUS 2, ADDRESS 0x50, with NO mux (measured on the appliance 2026-09-06: i2c-0 and
	 * i2c-2 exist, i2c-1 is status="disabled" in the DTB so a default of 1 opens NOTHING,
	 * both 0x50 and 0x51 answer on bus 2, and there is no mux at 0x70-0x77). Misreading it
	 * as "bus 1, channel 2" cost this project weeks and is still live in sfp-rate-read.sh. */
	uint8_t				i2c_bus;	/* /dev/i2c-<n> */
	uint8_t				i2c_a0;		/* SFF-8431 identity page   (0x50) */
	uint8_t				i2c_a2;		/* SFF-8472 diagnostics     (0x51) */
};

/*
 * The gpiochip LABEL the descriptor's `gpio:<bank>:` resolves to, or NULL if this file has no
 * MEASURED mapping for that bank — in which case the caller refuses rather than guessing.
 *
 * Exported, not private, for one reason: it is the only part of "resolve BY LABEL, never by a
 * base" that a unit test on a build host can actually MEASURE. A test that merely asserts
 * E_GPIO on a machine with no such gpiochip passes for the wrong reason and would keep passing
 * if the table were emptied. Asserting the bank table's contents directly cannot.
 */
const char *dp_sff_label_for_bank(uint8_t bank);

/* Bit N set => port device N has a cage on this board. Derived from the cage table, never a
 * constant: DP_SWOP_SFP_CAGE_MAP_XGS116 exists to CHECK this value, not to be it. */
uint16_t dp_sff_cage_map(void);

/* The cage owned by `dev`, or NULL if that port device has none (the wire's E_NOCAGE). */
const struct dp_sff_cage *dp_sff_cage_for_dev(uint8_t dev);

/* The pins this cage offers as MEASUREMENTS in phase A — the `implemented` mask. */
uint8_t dp_sff_implemented(const struct dp_sff_cage *cage);

/* Which of this cage's bits the descriptor inverts — the `active_low` mask. */
uint8_t dp_sff_active_low(const struct dp_sff_cage *cage);

/* ======================= THE A2 CROSS-CHECK ==================================================
 *
 * WHY. Every pin read in this file is a GPIO level, and NOTHING in it establishes that the pad
 * is actually in GPIO function. That is not a theoretical gap: on the sibling Nami board BOTH
 * polarities were shown to be contaminated when a pad's function is unknown — a pad held in a
 * PERIPHERAL function reads LOW as a GPIO, and an open-drain i2c pad with a pull-up and no
 * traffic IDLES HIGH, "which is exactly what an asserted active-high TX_DISABLE looks like".
 *
 *     ON A PAD WHOSE FUNCTION YOU HAVE NOT ESTABLISHED, NEITHER LEVEL IS EVIDENCE.
 *
 * That is the same defect this whole opcode exists to remove — a state indistinguishable from
 * another state, reported as the wrong one with full confidence — one layer further down.
 *
 * WHAT SETTLES IT. The MODULE reports two of the same four facts over a DIFFERENT TRANSPORT.
 * SFF-8472 puts a Status/Control byte at A2h (i2c 0x51) byte 110, and two of its bits are the
 * digital state of pins this board also wires to the SoC:
 *
 *     bit 7  TX Disable State   — the digital state of the TX_DISABLE *input pin*
 *     bit 1  RX_LOS State       — the digital state of the RX_LOS *output pin*
 *
 * So `rx_los` and `tx_disable` can be read a second way, and a DISAGREEMENT between the two is
 * the exact signature of a muxed or dead pad. A pin whose two transports disagree has NOT been
 * validly measured, so the handler CLEARS ITS `valid` BIT and leaves `implemented` set — the
 * existing contract's absence case (c) shape, and no new wire field.
 *
 * WHAT AGREEMENT DOES AND DOES NOT PROVE, because overclaiming here would be the same defect
 * wearing the other hat. For RX_LOS the SOURCE is the same on both paths (the module's own LOS
 * detector); the two paths differ in TRANSPORT, not in origin. So agreement proves THE PAD IS
 * CARRYING THE MODULE'S SIGNAL — which is precisely the question — and proves nothing new about
 * whether the module's detector is right. For TX_DISABLE the origin genuinely differs: WE drive
 * the pin, and the module reports what it sees ON that pin, so agreement there also closes the
 * loop on our own drive.
 *
 * WHICH PINS ARE NOT COVERED, said rather than implied. `present` is a BOARD signal (a cage
 * detect line); the module cannot report it and there is no A2 equivalent. `tx_fault` has an A2
 * bit (byte 110 bit 2) but it is the module's own view of its own fault, and this firmware does
 * not cross-check it. TWO of the four phase-A pins are cross-checked. The other two remain
 * single-transport readings and are not claimed otherwise anywhere.
 */

/* The pins this firmware can cross-check at all. Anything outside this is single-transport. */
#define DP_SFF_XCHK_PINS	((uint8_t)(DP_SWOP_SFP_PIN_RX_LOS | DP_SWOP_SFP_PIN_TX_DISABLE))

/* ---- SFF-8472 bit definitions. PUBLIC MSA ONLY -------------------------------------------
 *
 * Taken from the SFF-8472 "Diagnostic Monitoring Interface for Optical Transceivers" MSA and
 * from SFF-8431 for the pin semantics. NOT from any vendor source: this kit is the project's
 * first PUBLIC clean-room module and the owner's ruling is that bit semantics come from the
 * standards. Cited by table so a reader can check them against the spec rather than against us.
 */

/* A0h byte 92 — Diagnostic Monitoring Type (SFF-8472 Table 8-5). */
#define SFF8472_A0_DMT			92u
#define SFF8472_DMT_DDM_IMPLEMENTED	0x40u	/* bit 6: digital diagnostic monitoring exists.
						 * CLEAR => there is no A2 page to read and a
						 * "reading" from 0x51 would be an invention. */
#define SFF8472_DMT_ADDR_CHANGE_REQ	0x04u	/* bit 2: the address-change sequence is required
						 * to reach the diagnostics, i.e. A2 is NOT simply
						 * at 0x51. SET => refuse, do not improvise. */

/* A0h byte 93 — Enhanced Options (SFF-8472 Table 8-6). These are the module's own statement of
 * WHICH of byte 110's status bits it actually maintains, and they are the honest per-pin gate:
 * cross-checking against a bit the module declares it does not implement would MANUFACTURE
 * disagreements and clear the valid bit of a pin that was read perfectly well. */
#define SFF8472_A0_ENHANCED		93u
#define SFF8472_ENH_SOFT_TX_DIS		0x40u	/* bit 6: soft TX_DISABLE control AND monitoring */
#define SFF8472_ENH_SOFT_RX_LOS_MON	0x10u	/* bit 4: soft RX_LOS monitoring                 */

/* A2h byte 110 — Status/Control (SFF-8472 Table 9-11). Only the two PIN-STATE bits this
 * cross-check consults are named; the soft-control bits belong to the bring-up, not here. */
#define SFF8472_A2_STATUS		110u
#define SFF8472_SC_TX_DISABLE_PIN	0x80u	/* bit 7: digital state of the TX_DISABLE pin */
#define SFF8472_SC_RX_LOS_PIN		0x02u	/* bit 1: digital state of the RX_LOS pin     */

/* ---- INDEPENDENCE, AS A RUNTIME PROPERTY AND NOT AS A COMMENT -----------------------------
 *
 * A two-transport agreement test is only evidence IF THE TWO TRANSPORTS ARE INDEPENDENT, and
 * that property is the kind that gets quietly lost. TODAY it holds: the cage pins are lines on
 * an SoC GPIO block (the chip labelled "f2440000.system-controller:gpio@140", a CP0
 * system-controller block reached over MMIO), while the A2 read goes over the i2c controller
 * at /dev/i2c-2. Different silicon, different path to the same fact.
 *
 * WHAT WOULD BREAK IT: a later board — or a later change on this one — resolving those cage
 * pins through an I2C GPIO EXPANDER sitting on the same bus as the EEPROM. The "two transports"
 * would then be ONE READING COUNTED TWICE. The check would report AGREED, with total
 * confidence, on every pin, forever. IT COULD NOT FAIL, so it would look perfect — and a gate
 * that cannot fail is a missing gate that is trusted. THE TELL IS THAT IT NEVER DISAGREES.
 *
 * So it is ESTABLISHED at runtime, not asserted in prose: dp_sff.c walks the resolved
 * gpiochip's sysfs device parentage and reports which i2c adapter (if any) it hangs off, and
 * this predicate decides. UNKNOWN FAILS CLOSED — an undetermined parentage is not independence.
 */
#define DP_SFF_I2C_ADAPTER_NONE		(-1)	/* the gpiochip is on no i2c adapter at all */
#define DP_SFF_I2C_ADAPTER_UNKNOWN	(-2)	/* could not be determined — NOT independence */

/* 1 == the GPIO path and the i2c path are genuinely different transports. */
int dp_sff_xchk_independent(int gpio_i2c_adapter, int eeprom_i2c_bus);

/*
 * The facts a cross-check needs, GATHERED by the transport half (Linux i2c + sysfs) and
 * CLASSIFIED by the portable half. The split is deliberate: the classifier — where every
 * three-state decision lives — then runs on a build host, against constructed inputs, with no
 * hardware and no mock of the ioctl.
 */
struct dp_sff_xsrc {
	int	gpio_i2c_adapter;	/* >= 0 adapter number, or DP_SFF_I2C_ADAPTER_NONE /
					 * _UNKNOWN. See dp_sff_xchk_independent(). */
	int	a0_ok;			/* 1 == a0_dmt and a0_enh are REAL reads. An i2c failure
					 * is NOT a disagreement and must never become a 0x00:
					 * SFF-8472 gives 0x00 meanings, so a zero from a bus
					 * that did not answer decodes as a plausible, wrong,
					 * MODULE-SHAPED answer. */
	uint8_t	a0_dmt;			/* A0h byte 92 */
	uint8_t	a0_enh;			/* A0h byte 93 */
	int	a2_ok;			/* 1 == a2_status is a REAL read */
	uint8_t	a2_status;		/* A2h byte 110 */
};

/*
 * THREE STATES, AND THEY STAY THREE. Collapsing the third into either of the first two
 * re-commits the very defect this feature removes.
 *
 *   agreed     cross-checked, and the two transports AGREE   -> `valid` untouched
 *   disagreed  cross-checked, and they DISAGREE              -> `valid` bit CLEARED
 *   nocheck    NOT CROSS-CHECKABLE on this reply             -> `valid` untouched, and the
 *              reading stays exactly what it was: single-transport.
 *
 * The three are disjoint and their union is `want & DP_SFF_XCHK_PINS`.
 *
 * HONEST LIMIT OF THE WIRE, stated because it is a real one: `agreed` and `nocheck` are
 * INDISTINGUISHABLE in the reply, since both leave `valid` set and the five masks carry no
 * place to say which. The reply says "this is a measurement", not "this is a corroborated
 * measurement". Making that distinction visible needs a wire field, which is a dp_swop.h change
 * and is not taken here.
 */
struct dp_sff_xchk {
	uint8_t	agreed;
	uint8_t	disagreed;
	uint8_t	nocheck;
};

/*
 * Classify. Pure, portable, and the whole three-state decision.
 *
 *   logical  the NORMALISED (active-high) GPIO reading — the same domain the A2 status bits are
 *            in, so the comparison needs no polarity step. Comparing RAW levels here would
 *            reintroduce a polarity in a second place.
 *   want     the pins the caller wants corroborated (in practice `valid & DP_SFF_XCHK_PINS`).
 */
void dp_sff_xchk_classify(const struct dp_sff_xsrc *src, int eeprom_i2c_bus, uint8_t logical,
			  uint8_t want, struct dp_sff_xchk *out);

/*
 * THE COMPARATOR'S OWN PROOF THAT IT CAN FAIL, run as a STEP rather than trusted as a property.
 * Returns 1 only if the comparator rejects a KNOWN-BAD pair, accepts a KNOWN-GOOD one, and does
 * not answer for a pin it was not asked about. dp_sff_xchk_classify() calls this BEFORE it
 * classifies anything and falls back to all-nocheck if it returns 0 — so "the cross-check
 * passed" is a measurement on every single reply, not an assumption. (This project produced
 * four gates-that-could-not-fail in one day on 2026-09-04; the mechanical fix is exactly this.)
 */
int dp_sff_xchk_comparator_proven(void);

/*
 * Cross-check one cage's pins over the module's own A2 page. Gathers (Linux: i2c + sysfs) and
 * classifies. NEVER fails the reply: an unreachable module is the `nocheck` state, not an error
 * and not a disagreement.
 */
void dp_sff_cross_check(const struct dp_sff_cage *cage, uint8_t logical, uint8_t want,
			struct dp_sff_xchk *out);

/*
 * Read the cage's readable sideband lines.
 *
 *   *raw  the RAW electrical level of each line, in DP_SWOP_SFP_PIN_* bit positions, before
 *         any polarity normalisation. Bits outside *ok are left ZERO and mean nothing.
 *   *ok   the lines that were actually READ. A line that could not be read — claimed by a
 *         driver, or driven so that reading it would perturb it — clears its bit here, which
 *         is what keeps a floating or unreadable line from shipping as an electrical zero.
 *
 * Returns DP_SWOP_OK when the cage's gpiochip resolved BY LABEL (even if some individual lines
 * then failed — those show up in *ok), or DP_SWOP_E_GPIO when it did not. E_GPIO is deliberately
 * LOUD and has no fallback: a guessed chip base reads a DIFFERENT controller's lines and answers
 * with total confidence, which is the exact shape of the 88E6193X register-base bug (every port
 * read 0x0000 with the VALID bit SET, so a wrong address was indistinguishable from a genuinely
 * zero register).
 */
uint8_t dp_sff_read_pins(const struct dp_sff_cage *cage, uint8_t *raw, uint8_t *ok);

#ifdef DP_SWOP_TEST
/*
 * TEST SEAMS — compiled out entirely unless DP_SWOP_TEST is defined, exactly like dp_swop.c's
 * reg_read seam and for the same reason: the handler's ENVELOPE logic (which absence case a
 * given set of levels encodes) is unreachable on a host with no such gpiochip, where every read
 * can only ever be E_GPIO.
 *
 * These do NOT mock the ioctl. The chardev/sysfs transaction is untested here and stated as
 * untested; a mock of it would only assert that this file agrees with itself.
 */

/* Substitute the line read, so the four absence cases become observable. NULL restores. */
void dp_sff_test_set_read(uint8_t (*fn)(const struct dp_sff_cage *cage, uint8_t *raw,
					uint8_t *ok));

/*
 * Substitute the CROSS-CHECK's gathered facts, so the three states — agreed, disagreed, and
 * NOT CROSS-CHECKABLE — become observable through the real handler on a build host. It replaces
 * the TRANSPORT (i2c reads + sysfs parentage walk) and NOT the classifier: every three-state
 * decision under test is the shipping one. NULL restores the real gather.
 *
 * The shared-device case — a gpiochip hanging off the SAME i2c adapter as the EEPROM — is
 * constructed through this seam, because it is the case that will still be true after somebody
 * moves these pins onto an expander, and it cannot otherwise be reached from any host we own.
 */
void dp_sff_test_set_xsrc(const struct dp_sff_xsrc *src);

/* Override the label a bank resolves to, so the NEGATIVE control ("an unresolvable gpiochip
 * label fails loudly") is a MEASUREMENT rather than an accident of the host it ran on. Without
 * it the real label happens not to exist on a build host, which proves nothing about a box
 * where it does. NULL restores the board's own label. */
void dp_sff_test_set_chip_label(const char *label);
#endif

#endif /* DP_SFF_H */
