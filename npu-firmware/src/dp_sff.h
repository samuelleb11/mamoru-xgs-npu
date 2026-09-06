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
	/* No i2c bus or address field. Phase B adds one HERE, on the side of the wire where the
	 * descriptor lives — and it must be parsed with the vendor's literal "i2c1:%i:%i", where
	 * `i2c1` is a FIXED PREFIX TOKEN and the FIRST number is the BUS. `i2c1:2:0x50` is bus 2
	 * address 0x50 with NO mux (measured 2026-09-06). Misreading it as "bus 1, channel 2"
	 * cost this project weeks and is still live in sfp-rate-read.sh. */
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

/* Override the label a bank resolves to, so the NEGATIVE control ("an unresolvable gpiochip
 * label fails loudly") is a MEASUREMENT rather than an accident of the host it ran on. Without
 * it the real label happens not to exist on a build host, which proves nothing about a box
 * where it does. NULL restores the board's own label. */
void dp_sff_test_set_chip_label(const char *label);
#endif

#endif /* DP_SFF_H */
