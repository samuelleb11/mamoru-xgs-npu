/* SPDX-License-Identifier: MIT
 *
 * dp_sff.c — the XGS 116's SFP cage descriptor, and the sideband GPIO reads behind
 * DP_SWOP_OP_SFP phase A.
 *
 * WHY THIS EXISTS. On 2026-09-06 the appliance's only optical port was dark for three days.
 * The answer — the far end's laser was off — was two bytes of sideband this board has had
 * wired since it was built and nothing in our software has ever read. `rx_los` says "no light
 * is arriving" in one glance.
 *
 * ================ GPIO ACCESS: WHICH INTERFACE, AND WHY ================
 *
 * NOT the vendor `libsbsp.so`. It is vendor code, this kit is our first PUBLIC clean-room
 * module, and `libsbsp.so.1` is in any case MISSING on the appliance — `dp_swctl` dies with
 * "error while loading shared libraries: libsbsp.so.1", rc=127. Linking it would trade a
 * clean-room boundary for a dependency that does not resolve on the target.
 *
 * THE NPU RUNS LINUX 4.14, and that decides the interface:
 *
 *   GPIO chardev v2 — GPIO_V2_GET_LINE_IOCTL, struct gpio_v2_line_request — arrived in Linux
 *   5.10. It DOES NOT EXIST on 4.14 and is NOT USED here. Reaching for it because it is the
 *   modern spelling would compile perfectly on any build host and fail only on the appliance.
 *
 *   GPIO chardev v1 — GPIO_GET_CHIPINFO_IOCTL, GPIO_GET_LINEHANDLE_IOCTL,
 *   GPIOHANDLE_GET_LINE_VALUES_IOCTL, struct gpiochip_info / gpiohandle_request /
 *   gpiohandle_data — arrived in Linux 4.8, i.e. two years before 4.14. That is what this file
 *   uses, and it uses NOTHING added after 4.8 on that path.
 *
 *   The /sys/class/gpio interface predates both and is still present in 4.14. It is the
 *   FALLBACK, per line, for two cases the chardev cannot serve: a box whose /dev/gpiochip*
 *   nodes are absent (devtmpfs/udev is not something this file gets to assume — the appliance
 *   reading we have enumerated the chips through SYSFS, and says nothing about /dev), and a
 *   line already exported by sfp-init.sh's bring-up, which a chardev request would refuse with
 *   -EBUSY while sysfs reads it happily.
 *
 * ================ RESOLUTION IS BY LABEL. THERE IS NO FALLBACK TO A BASE. ================
 *
 * Measured 2026-09-06: the booted DTB maps the descriptor's `gpio2` to
 * /cp0/config-space/system-controller@440000/gpio@140, status="okay", ngpios 31 — the chip
 * whose sysfs label is "f2440000.system-controller:gpio@140", which was Linux gpiochip64 ON
 * THAT BOOT. The base is NOT a stable name: Linux GPIO bases move across kernels, and the
 * chardev's own numbering is different again (/dev/gpiochipN is a registration INDEX, not a
 * base — on this box the three chips are sysfs gpiochip0/32/64 and would be chardev
 * gpiochip0/1/2). Nothing on the appliance is exported, there are no gpio-line-names, and
 * debugfs is not mounted, so a guessed base has nothing to contradict it: it reads a DIFFERENT
 * controller's lines and answers with total confidence. That is precisely the shape of the
 * 88E6193X register-base bug that cost this project weeks — every port read 0x0000 with the
 * VALID bit SET, so a wrong address was indistinguishable from a genuinely zero register.
 *
 * So: match the LABEL, and if no chip carries it, answer DP_SWOP_E_GPIO and stop.
 */

/* Include guard on the whole body: this file is #included by dp_swop.c to keep dp_fwd a single
 * translation unit (see dp_sff.h), so a build that also compiles it separately would otherwise
 * duplicate every symbol. */
#ifndef DP_SFF_C
#define DP_SFF_C

#include "dp_sff.h"

#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
/* The KERNEL's UAPI i2c headers, not i2c-tools'. `linux/i2c.h` carries struct i2c_msg and
 * I2C_M_RD; `linux/i2c-dev.h` carries I2C_RDWR and struct i2c_rdwr_ioctl_data. Both have been
 * exported UAPI since long before 4.14, so this adds no package dependency to the NPU image --
 * which matters, because the vendor `libsbsp.so.1` this kit deliberately does not link is
 * MISSING on the appliance and a second unresolvable dependency would be the same mistake. */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define SFF_COUNT(a)	((uint8_t)(sizeof(a) / sizeof((a)[0])))

/* ---- THE BOARD TABLE ----------------------------------------------------------------------
 *
 * Ground truth, AMDA0208-0001R00.txt:163-171 (front panel F1 == switch port device 0x09 ==
 * vendor npu0.eth8/phy8):
 *
 *     npu0.phy8.sff=8431
 *     npu0.phy8.pin.tx_fault=gpio:2:21
 *     npu0.phy8.pin.rate_select_1=gpio:2:29
 *     npu0.phy8.pin.rate_select_0=gpio:2:23
 *     npu0.phy8.pin.rx_los=gpio:2:28
 *     npu0.phy8.pin.tx_disable=gpio:2:17
 *     npu0.phy8.pin.present=-gpio:2:22
 *
 * The leading minus on `present` — and ONLY on `present` — is the active-low marker.
 */
static const struct dp_sff_pin sff_xgs116_f1_pins[] = {
	/*  bit                          bank line  a_low  read  driven */
	{ DP_SWOP_SFP_PIN_PRESENT,	2,   22,    1,    1,     0 },
	{ DP_SWOP_SFP_PIN_RX_LOS,	2,   28,    0,    1,     0 },
	{ DP_SWOP_SFP_PIN_TX_FAULT,	2,   21,    0,    1,     0 },
	{ DP_SWOP_SFP_PIN_TX_DISABLE,	2,   17,    0,    1,     1 },
	/* RATE SELECT: wired, and phase A DRIVES both at bring-up, but phase A never reads them
	 * back — so `phase_a_read` is 0 and they stay OUT of `implemented`. The wire contract is
	 * explicit about this: claiming a pin is implemented while reporting a value nobody read
	 * is absence case (d) wearing case (c)'s clothes, which is the confusion this opcode
	 * exists to prevent. They are listed anyway because the BOARD wires them; adding the
	 * readback later is one flag here, not a wire change. */
	{ DP_SWOP_SFP_PIN_RS0,		2,   23,    0,    0,     1 },
	{ DP_SWOP_SFP_PIN_RS1,		2,   29,    0,    0,     1 }
};

/* One cage on this board. A second entry — another port, or another board's file — is the only
 * change a multi-cage box needs, which is why the wire contract is per-port. */
static const struct dp_sff_cage sff_cages[] = {
	/*                                                       bus   A0     A2
	 * Ground truth, AMDA0208-0001R00.txt: `npu0.phy8.SFF-8431=i2c1:2:0x50`, parsed with the
	 * vendor's literal "i2c1:%i:%i" -- `i2c1` is a FIXED PREFIX TOKEN and the FIRST number is
	 * the BUS. So: /dev/i2c-2, A0 at 0x50, A2 at 0x51, NO MUX. Measured on the appliance
	 * 2026-09-06: both addresses answer i2cdetect on bus 2, `i2cget -y 2 0x51 0x6e` returned
	 * 0x02, and /dev/i2c-1 DOES NOT EXIST (that controller is status="disabled" in the DTB),
	 * so a default of bus 1 would open nothing. A2 is 0x51 = A0 + 1 by SFF-8472, but it is
	 * written out rather than derived: a derived address is a second convention to get wrong.
	 */
	{ 0x09, sff_xgs116_f1_pins, SFF_COUNT(sff_xgs116_f1_pins), 2, 0x50, 0x51 }
};

/*
 * Bank -> gpiochip LABEL. Only bank 2 is here, and its absence of siblings is deliberate: bank
 * 2 is the one whose mapping was MEASURED from the booted DTB. The box has two other gpiochips
 * ("f06f4000.system-controller:gpio@1040", 20 lines, and "f2440000.system-controller:gpio@100",
 * 32 lines) and it is TEMPTING to assume they are the descriptor's banks 0 and 1 — but nobody
 * has read that out of the DTB, and a plausible guess about which controller a pin lives on is
 * exactly the failure this file refuses to have. An unlisted bank answers E_GPIO.
 */
struct sff_bank {
	uint8_t		bank;
	const char     *label;
};

static const struct sff_bank sff_banks[] = {
	{ 2, "f2440000.system-controller:gpio@140" }	/* measured 2026-09-06; ngpio 31 */
};

#ifdef DP_SWOP_TEST
static const char *sff_label_override;
#endif

const char *dp_sff_label_for_bank(uint8_t bank)
{
	uint8_t i;

#ifdef DP_SWOP_TEST
	if (sff_label_override)
		return sff_label_override;
#endif
	for (i = 0; i < SFF_COUNT(sff_banks); i++)
		if (sff_banks[i].bank == bank)
			return sff_banks[i].label;
	return NULL;
}

/* ---- the descriptor, as masks -------------------------------------------------------------- */

uint16_t dp_sff_cage_map(void)
{
	uint16_t map = 0;
	uint8_t i;

	for (i = 0; i < SFF_COUNT(sff_cages); i++)
		map = (uint16_t)(map | (uint16_t)(1u << sff_cages[i].dev));
	return map;
}

const struct dp_sff_cage *dp_sff_cage_for_dev(uint8_t dev)
{
	uint8_t i;

	for (i = 0; i < SFF_COUNT(sff_cages); i++)
		if (sff_cages[i].dev == dev)
			return &sff_cages[i];
	return NULL;
}

/*
 * `implemented` is the set of pins that MAY appear in `valid` — i.e. the pins this firmware
 * actually reads — not the set the board merely wires. On this board the two coincide for all
 * four phase-A bits, so the distinction costs nothing here and buys the honest answer on a
 * board where a phase-A pin is wired but unreadable: that is absence case (c), implemented
 * CLEAR, and NOT a pin claimed and then reported as an unmeasured zero.
 *
 * Masked to DP_SWOP_SFP_PINS_PHASE_A so a table entry that gained a read flag without the wire
 * contract gaining the bit cannot leak a reserved bit onto the wire.
 */
uint8_t dp_sff_implemented(const struct dp_sff_cage *cage)
{
	uint8_t m = 0, i;

	if (!cage)
		return 0;
	for (i = 0; i < cage->npins; i++)
		if (cage->pins[i].phase_a_read)
			m = (uint8_t)(m | cage->pins[i].bit);
	return (uint8_t)(m & DP_SWOP_SFP_PINS_PHASE_A);
}

/*
 * `active_low` is BOARD truth and covers every pin in the table, read or not: the host must
 * never hold polarity of its own, and the field's job is to let it re-derive
 * `logical == raw ^ active_low` for the bits it was given. Bits outside `valid` are ignored by
 * that check, so a polarity for a pin nobody read is harmless — and dropping it would make the
 * field mean something different from what the contract says it means.
 */
uint8_t dp_sff_active_low(const struct dp_sff_cage *cage)
{
	uint8_t m = 0, i;

	if (!cage)
		return 0;
	for (i = 0; i < cage->npins; i++)
		if (cage->pins[i].active_low)
			m = (uint8_t)(m | cage->pins[i].bit);
	return m;
}

/* ---- THE CROSS-CHECK, PORTABLE HALF -------------------------------------------------------
 *
 * Everything that DECIDES anything lives here and compiles everywhere, so the three-state logic
 * is exercised by the unit suite on a build host rather than only on the appliance. The Linux
 * half below is transport only: it gathers bytes and a parentage, and decides nothing.
 */

/*
 * Which of `pins` the two transports DISAGREE about.
 *
 * `logical` is the NORMALISED, active-high GPIO reading; the A2 status bits are already in that
 * domain (SFF-8472 defines them as the digital state of the pin, and both of these pins are
 * active high in the SFF sense), so there is no polarity step here and deliberately so -- a
 * second place holding polarity is a second place for it to be wrong.
 */
static uint8_t sff_xchk_compare(uint8_t a2_status, uint8_t logical, uint8_t pins)
{
	uint8_t a2 = 0;

	if (a2_status & SFF8472_SC_RX_LOS_PIN)
		a2 = (uint8_t)(a2 | DP_SWOP_SFP_PIN_RX_LOS);
	if (a2_status & SFF8472_SC_TX_DISABLE_PIN)
		a2 = (uint8_t)(a2 | DP_SWOP_SFP_PIN_TX_DISABLE);
	return (uint8_t)((a2 ^ logical) & pins);
}

int dp_sff_xchk_comparator_proven(void)
{
	/* KNOWN-BAD: A2 says RX_LOS asserted, the GPIO says clear. Must DISAGREE. */
	if (sff_xchk_compare(SFF8472_SC_RX_LOS_PIN, 0, DP_SWOP_SFP_PIN_RX_LOS) !=
	    DP_SWOP_SFP_PIN_RX_LOS)
		return 0;
	/* KNOWN-GOOD: both say asserted. Must AGREE. A comparator that only ever returns
	 * "disagree" would pass the row above and fail here. */
	if (sff_xchk_compare(SFF8472_SC_RX_LOS_PIN, DP_SWOP_SFP_PIN_RX_LOS,
			     DP_SWOP_SFP_PIN_RX_LOS) != 0)
		return 0;
	/* ...and both again for TX_DISABLE, whose A2 bit is at a DIFFERENT position (7, not 1).
	 * One pin's rows passing says nothing about the other's bit being mapped correctly. */
	if (sff_xchk_compare(SFF8472_SC_TX_DISABLE_PIN, 0, DP_SWOP_SFP_PIN_TX_DISABLE) !=
	    DP_SWOP_SFP_PIN_TX_DISABLE)
		return 0;
	if (sff_xchk_compare(SFF8472_SC_TX_DISABLE_PIN, DP_SWOP_SFP_PIN_TX_DISABLE,
			     DP_SWOP_SFP_PIN_TX_DISABLE) != 0)
		return 0;
	/* CROSS-WIRING CONTROL: one pin's A2 bit must not answer for the OTHER pin. A
	 * transposition of the two bit positions passes every row above and fails this one. */
	if (sff_xchk_compare(SFF8472_SC_RX_LOS_PIN, 0, DP_SWOP_SFP_PIN_TX_DISABLE) != 0)
		return 0;
	if (sff_xchk_compare(SFF8472_SC_TX_DISABLE_PIN, 0, DP_SWOP_SFP_PIN_RX_LOS) != 0)
		return 0;
	/* AND IT MUST NOT ANSWER FOR A PIN IT WAS NOT ASKED ABOUT. `pins` is a real mask, not a
	 * hint: a comparator ignoring it would clear the valid bit of a pin nobody cross-checked. */
	if (sff_xchk_compare(0xffu, 0x00u, 0) != 0)
		return 0;
	return 1;
}

int dp_sff_xchk_independent(int gpio_i2c_adapter, int eeprom_i2c_bus)
{
	/* UNDETERMINED IS NOT INDEPENDENT. This is the fail-closed direction on purpose: the
	 * cost of being wrong here is a check that CANNOT FAIL and is trusted, which is strictly
	 * worse than no check. The cost of being wrong the other way is a pin that stays a
	 * single-transport reading -- exactly what it was before this feature existed. */
	if (gpio_i2c_adapter == DP_SFF_I2C_ADAPTER_UNKNOWN)
		return 0;
	/* The gpiochip is not on any i2c adapter at all -- today's XGS 116 case, where the pins
	 * are lines on a CP0 system-controller block reached over MMIO. Different silicon. */
	if (gpio_i2c_adapter == DP_SFF_I2C_ADAPTER_NONE)
		return 1;
	/* It IS an i2c-backed gpiochip. Independent only if it is on a DIFFERENT adapter from the
	 * one carrying the EEPROM. Same adapter == ONE READING COUNTED TWICE. */
	return gpio_i2c_adapter != eeprom_i2c_bus;
}

void dp_sff_xchk_classify(const struct dp_sff_xsrc *src, int eeprom_i2c_bus, uint8_t logical,
			  uint8_t want, struct dp_sff_xchk *out)
{
	uint8_t cando = 0, diff;

	out->agreed = 0;
	out->disagreed = 0;
	/* START PESSIMISTIC. Every pin asked about is NOT CROSS-CHECKABLE until something below
	 * moves it, so every `return` on this function's failure paths lands in the third state
	 * by construction rather than by remembering to. */
	out->nocheck = (uint8_t)(want & DP_SFF_XCHK_PINS);
	if (!out->nocheck || !src)
		return;

	/* THE GATE PROVES IT CAN FAIL, ON THIS REPLY, BEFORE IT IS BELIEVED. Costs a handful of
	 * instructions and converts "the comparator works" from an assumption into a
	 * measurement -- in production, not in a test nobody reruns. */
	if (!dp_sff_xchk_comparator_proven())
		return;

	/* INDEPENDENCE. If the gpiochip hangs off the same i2c adapter as the EEPROM, these are
	 * not two transports and an "agreement" would be a number compared with itself. */
	if (!dp_sff_xchk_independent(src->gpio_i2c_adapter, eeprom_i2c_bus))
		return;

	/* AN i2c FAILURE IS NOT A DISAGREEMENT. No module, a bus NAK, an adapter that will not
	 * open: each leaves the GPIO reading as the only evidence and must NOT clear `valid`. And
	 * a byte that was never read must never be substituted with 0x00 -- SFF-8472 gives 0x00
	 * meanings, so a zero from a bus that did not answer decodes as a plausible wrong answer.
	 * That substitution is impossible here because a2_status is consulted only under a2_ok. */
	if (!src->a0_ok || !src->a2_ok)
		return;

	/* DOES THE MODULE HAVE DIAGNOSTICS AT ALL? A0h byte 92 bit 6 (SFF-8472 Table 8-5). A
	 * module without DDM has no A2 page, so anything 0x51 returned is not a status byte. */
	if (!(src->a0_dmt & SFF8472_DMT_DDM_IMPLEMENTED))
		return;
	/* ...and if byte 92 bit 2 says the address-change sequence is REQUIRED, the diagnostics
	 * are not simply at 0x51 and this read addressed something else. Refuse; do not improvise
	 * an access method against a module that told us the plain one is wrong. */
	if (src->a0_dmt & SFF8472_DMT_ADDR_CHANGE_REQ)
		return;

	/* PER-PIN CAPABILITY, from the module's own Enhanced Options byte (A0h byte 93, SFF-8472
	 * Table 8-6). A module that does not maintain byte 110's RX_LOS or TX_DISABLE state bit
	 * would otherwise MANUFACTURE a disagreement and clear the valid bit of a pin that was
	 * read perfectly well -- the false-FAIL mirror of the false-PASS this feature removes. */
	if (src->a0_enh & SFF8472_ENH_SOFT_RX_LOS_MON)
		cando = (uint8_t)(cando | DP_SWOP_SFP_PIN_RX_LOS);
	if (src->a0_enh & SFF8472_ENH_SOFT_TX_DIS)
		cando = (uint8_t)(cando | DP_SWOP_SFP_PIN_TX_DISABLE);
	cando = (uint8_t)(cando & out->nocheck);
	if (!cando)
		return;

	diff = sff_xchk_compare(src->a2_status, logical, cando);
	out->disagreed = diff;
	out->agreed = (uint8_t)(cando & (uint8_t)~(unsigned)diff);
	out->nocheck = (uint8_t)(out->nocheck & (uint8_t)~(unsigned)cando);
}

/* ---- line access --------------------------------------------------------------------------- */

/*
 * A resolved bank: whichever of the two interfaces found the chip whose LABEL matches. Both may
 * resolve; a per-line read tries the chardev first and falls back to sysfs, so a line the
 * chardev refuses (-EBUSY, because bring-up exported it) is still readable.
 *
 * Cached across calls so an OP_SFP does not re-scan every time, and INVALIDATED rather than
 * cached on failure so a chip that appears later is still found. `resolved` is the only thing
 * that says a lookup happened; a zeroed cache means "not looked up yet", never "not there".
 */
struct sff_chip {
	int	resolved;	/* 1 == a lookup ran for `bank` */
	uint8_t	bank;
	int	fd;		/* chardev fd, or -1 */
	int	base;		/* sysfs base, or -1 */
	/* The chip's own directory under /sys/class/gpio, e.g. "gpiochip64", or "" if the sysfs
	 * half did not resolve. Kept because the INDEPENDENCE walk needs the chip's DEVICE
	 * PARENTAGE and that is only reachable through sysfs -- see sff_gpiochip_i2c_adapter().
	 * The name is not a base and nothing addresses a line through it. */
	char	dir[40];
};

static struct sff_chip sff_chip_cache = { 0, 0, -1, -1, { 0 } };

static void sff_chip_forget(void)
{
#if defined(__linux__)
	if (sff_chip_cache.fd >= 0)
		close(sff_chip_cache.fd);
#endif
	sff_chip_cache.resolved = 0;
	sff_chip_cache.bank = 0;
	sff_chip_cache.fd = -1;
	sff_chip_cache.base = -1;
	sff_chip_cache.dir[0] = '\0';
}

#if defined(__linux__)

/* /dev/gpiochipN's N is a registration INDEX, not a base — see the header comment. A bounded
 * scan, because this runs on dp_fwd's management pump where an unbounded loop starves the host
 * handshake and tears the link down (the same rule dp_swop.c's SPIN_BUDGET enforces). */
#define SFF_CHARDEV_SCAN_MAX	32

/* Names this process in the kernel's line-consumer field, so `cat /sys/kernel/debug/gpio` on a
 * box where debugfs IS mounted attributes a held line to us rather than to "?". */
static const char sff_consumer[] = "dp_fwd-sfp";
#ifdef __GNUC__
_Static_assert(sizeof(sff_consumer) <=
	       sizeof(((struct gpiohandle_request *)0)->consumer_label),
	       "consumer label must fit the kernel's fixed field");
#endif

static int sff_read_file(const char *path, char *buf, size_t len)
{
	int fd, n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return n;
}

static int sff_write_file(const char *path, const char *text)
{
	int fd, n;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)write(fd, text, strlen(text));
	close(fd);
	return n < 0 ? -1 : 0;
}

/* Chardev: walk /dev/gpiochip* and compare GPIO_GET_CHIPINFO_IOCTL's label. 4.8+. */
static int sff_chardev_by_label(const char *label)
{
	char path[32];
	struct gpiochip_info info;
	int i, fd;

	for (i = 0; i < SFF_CHARDEV_SCAN_MAX; i++) {
		snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
			info.label[sizeof(info.label) - 1] = '\0';
			if (strcmp(info.label, label) == 0)
				return fd;
		}
		close(fd);
	}
	return -1;
}

/* Sysfs: walk /sys/class/gpio/gpiochip*, compare `label`, and take the BASE from that chip's
 * own `base` file rather than from the directory name — same rule, one interface down. */
static int sff_sysfs_base_by_label(const char *label, char *dir_out, size_t dir_len)
{
	char path[128], val[64], name[40];
	struct dirent *e;
	size_t n;
	DIR *d;
	int base = -1;

	if (dir_out && dir_len)
		dir_out[0] = '\0';

	d = opendir("/sys/class/gpio");
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "gpiochip", 8) != 0)
			continue;
		/* Bound the name into a fixed buffer before it reaches a format string. A
		 * directory entry is 255 bytes wide as declared, and GCC is right to refuse
		 * an snprintf that could truncate: a SILENTLY TRUNCATED path is a path to a
		 * different file, which in this file means resolving the wrong chip -- the
		 * exact failure the label lookup exists to prevent. An over-long name is not
		 * a gpiochip we know how to name, so it is skipped, not guessed at.
		 *
		 * FOUND ON LINUX, NOT HERE: clang on the build Mac has no
		 * -Wformat-truncation, so this passed every local gate and failed only on
		 * mamoru-dev01. It is the project's cfg-split rule in miniature -- the half
		 * that only compiles on the target is the half no host gate ever sees. */
		n = strlen(e->d_name);
		if (n >= sizeof(name))
			continue;
		memcpy(name, e->d_name, n + 1);
		snprintf(path, sizeof(path), "/sys/class/gpio/%s/label", name);
		if (sff_read_file(path, val, sizeof(val)) < 0)
			continue;
		if (strcmp(val, label) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/gpio/%s/base", name);
		if (sff_read_file(path, val, sizeof(val)) > 0) {
			base = (int)strtol(val, NULL, 10);
			if (dir_out && dir_len > n)
				memcpy(dir_out, name, n + 1);
		}
		break;
	}
	closedir(d);
	return base;
}

/*
 * One line, over the chardev. `driven` is a SAFETY input, not a hint.
 *
 * v1's GPIOHANDLE_REQUEST_INPUT does not merely say "I intend to read": it calls
 * gpiod_direction_input() on the line. Doing that to `tx_disable` — an OUTPUT the bring-up
 * drives — would release the drive on a laser-enable pin as a side effect of reading telemetry.
 * So the request is made with NO direction flag, which v1 accepts and which leaves the line's
 * direction alone (an output line then reads back its DRIVEN value, which is exactly the datum
 * `tx_disable` is supposed to report). Only if that is refused, and only for a line the board
 * does NOT drive, is INPUT tried. A driven line that cannot be read as-is clears its `valid`
 * bit; it is never coerced.
 */
static int sff_chardev_read_line(int chipfd, uint8_t line, uint8_t driven, int *out)
{
	struct gpiohandle_request rq;
	struct gpiohandle_data data;
	int attempt, rc;

	for (attempt = 0; attempt < 2; attempt++) {
		if (attempt == 1 && driven)
			return -1;		/* never switch a driven line to input */
		memset(&rq, 0, sizeof(rq));
		rq.lineoffsets[0] = line;
		rq.lines = 1;
		rq.flags = (attempt == 0) ? 0u : (uint32_t)GPIOHANDLE_REQUEST_INPUT;
		memcpy(rq.consumer_label, sff_consumer, sizeof(sff_consumer));
		rq.fd = -1;
		if (ioctl(chipfd, GPIO_GET_LINEHANDLE_IOCTL, &rq) < 0 || rq.fd < 0)
			continue;
		memset(&data, 0, sizeof(data));
		rc = ioctl(rq.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
		close(rq.fd);
		if (rc < 0)
			return -1;
		*out = data.values[0] ? 1 : 0;
		return 0;
	}
	return -1;
}

/*
 * One line, over sysfs. Export does NOT change direction (gpiod_export leaves it as it found
 * it), so this is non-perturbing for `tx_disable` too. A line already exported is read WITHOUT
 * exporting and WITHOUT unexporting it — it belongs to somebody else, most likely sfp-init.sh's
 * bring-up, and unexporting it underneath them would be a side effect of a read.
 */
static int sff_sysfs_read_line(int base, uint8_t line, int *out)
{
	char vpath[64], num[16], val[16];
	int mine = 0, rc = -1;

	snprintf(num, sizeof(num), "%d", base + (int)line);
	snprintf(vpath, sizeof(vpath), "/sys/class/gpio/gpio%s/value", num);

	if (access(vpath, R_OK) != 0) {
		if (sff_write_file("/sys/class/gpio/export", num) < 0)
			return -1;
		mine = 1;
	}
	if (sff_read_file(vpath, val, sizeof(val)) > 0) {
		*out = (val[0] == '0') ? 0 : 1;
		rc = 0;
	}
	if (mine)
		(void)sff_write_file("/sys/class/gpio/unexport", num);
	return rc;
}

static const struct sff_chip *sff_chip_for_bank(uint8_t bank)
{
	const char *label;

	if (sff_chip_cache.resolved && sff_chip_cache.bank == bank)
		return &sff_chip_cache;

	sff_chip_forget();
	label = dp_sff_label_for_bank(bank);
	if (!label)
		return NULL;		/* an unlisted bank is a guess we refuse to make */

	sff_chip_cache.bank = bank;
	sff_chip_cache.fd = sff_chardev_by_label(label);
	sff_chip_cache.base = sff_sysfs_base_by_label(label, sff_chip_cache.dir,
						      sizeof(sff_chip_cache.dir));
	if (sff_chip_cache.fd < 0 && sff_chip_cache.base < 0) {
		/* Not cached: a NEGATIVE result must not become permanent, or a chip that
		 * appears after a later module load would never be found. */
		sff_chip_cache.bank = 0;
		return NULL;
	}
	sff_chip_cache.resolved = 1;
	return &sff_chip_cache;
}

static int sff_read_line(const struct sff_chip *chip, uint8_t line, uint8_t driven, int *out)
{
	if (chip->fd >= 0 && sff_chardev_read_line(chip->fd, line, driven, out) == 0)
		return 0;
	if (chip->base >= 0 && sff_sysfs_read_line(chip->base, line, out) == 0)
		return 0;
	return -1;
}

/* ---- THE CROSS-CHECK, LINUX TRANSPORT HALF ------------------------------------------------
 *
 * Gathers bytes and a device parentage. DECIDES NOTHING -- every three-state judgement is
 * dp_sff_xchk_classify()'s, above, which compiles and is tested on a build host.
 */

/*
 * One short read from an i2c device: write the byte offset, repeated START, read `len` bytes.
 * I2C_RDWR rather than the SMBus emulation, because that is exactly the two-message transaction
 * an SFF page read is and it does not depend on the adapter exposing SMBus functionality.
 *
 * A FAILURE RETURNS -1 AND WRITES NOTHING. It never returns a zero byte: SFF-8472 assigns
 * meanings to 0x00, so a zero manufactured from a bus that did not answer decodes as a
 * plausible, wrong, module-shaped answer -- which is the entire defect this file exists to
 * remove, re-committed one transport over. On this hardware an absent device is a LOUD EIO
 * (measured 2026-09-06: `i2cget` on an unpopulated address returns rc=1, not a zero).
 *
 * BOUNDED WORK. This runs on dp_fwd's management pump, where an unbounded operation starves the
 * host handshake and tears the link down (the rule dp_swop.c's SPIN_BUDGET enforces). A whole
 * cross-check is TWO transactions of at most three bytes each; the fd is opened and closed per
 * call rather than cached, so a swapped module or a re-probed adapter is never held stale.
 */
static int sff_i2c_read(uint8_t bus, uint8_t addr, uint8_t off, uint8_t *buf, uint8_t len)
{
	char path[32];
	struct i2c_msg msg[2];
	struct i2c_rdwr_ioctl_data xfer;
	uint8_t reg = off;
	int fd, rc;

	snprintf(path, sizeof(path), "/dev/i2c-%u", (unsigned)bus);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;		/* the adapter is not there -- e.g. the DTB disabled it */
	memset(msg, 0, sizeof(msg));
	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &reg;
	msg[1].addr = addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;
	xfer.msgs = msg;
	xfer.nmsgs = 2;
	rc = ioctl(fd, I2C_RDWR, &xfer);
	close(fd);
	return rc < 0 ? -1 : 0;
}

/*
 * WHICH i2c ADAPTER, IF ANY, THE RESOLVED GPIOCHIP HANGS OFF -- the runtime half of the
 * independence property. This is the thing that stops the cross-check quietly becoming one
 * reading counted twice on the day somebody moves these pins to an i2c GPIO expander.
 *
 * /sys/class/gpio/gpiochipN is a SYMLINK into /sys/devices/..., and the components of that link
 * ARE the device's parentage, all the way up. An i2c-expander-backed gpiochip therefore has an
 * `i2c-<n>` component in its path (the adapter), typically followed by an `<n>-00xx` client; an
 * MMIO system-controller block, which is what this board actually has, has none. Reading the
 * link is one syscall and needs no realpath, no /sys/bus/gpio (which 4.14 may not have) and no
 * assumption about how deep the expander sits.
 *
 * IT REQUIRES THE SYSFS HALF TO HAVE RESOLVED. If only the chardev found the chip there is no
 * class directory to walk, and the answer is UNKNOWN -- which dp_sff_xchk_independent() treats
 * as NOT independent. Fail closed: a pin that stays single-transport is what it was yesterday;
 * a cross-check that cannot fail is worse than none.
 */
static int sff_gpiochip_i2c_adapter(const char *chipdir)
{
	char path[96], link[512];
	const char *c;
	ssize_t n;

	if (!chipdir || !chipdir[0])
		return DP_SFF_I2C_ADAPTER_UNKNOWN;
	snprintf(path, sizeof(path), "/sys/class/gpio/%s", chipdir);
	n = readlink(path, link, sizeof(link) - 1);
	if (n <= 0)
		return DP_SFF_I2C_ADAPTER_UNKNOWN;
	if ((size_t)n >= sizeof(link) - 1)
		return DP_SFF_I2C_ADAPTER_UNKNOWN;	/* truncated == parentage not seen */
	link[n] = '\0';

	for (c = link; *c != '\0'; ) {
		if (strncmp(c, "i2c-", 4) == 0 && c[4] >= '0' && c[4] <= '9') {
			int adapter = 0;
			const char *d = c + 4;

			while (*d >= '0' && *d <= '9') {
				adapter = adapter * 10 + (*d - '0');
				d++;
				if (adapter > 65535)
					return DP_SFF_I2C_ADAPTER_UNKNOWN;
			}
			/* Only a WHOLE path component counts. "i2c-2" is an adapter;
			 * "i2c-2xyz" is some other device whose name happens to start that
			 * way, and treating it as one would be a guess. */
			if (*d == '/' || *d == '\0')
				return adapter;
		}
		while (*c != '\0' && *c != '/')
			c++;
		while (*c == '/')
			c++;
	}
	return DP_SFF_I2C_ADAPTER_NONE;
}

static void sff_xchk_gather(const struct dp_sff_cage *cage, struct dp_sff_xsrc *src)
{
	const struct sff_chip *chip;
	uint8_t a0[2];
	int bank = -1;
	uint8_t i;

	src->gpio_i2c_adapter = DP_SFF_I2C_ADAPTER_UNKNOWN;
	src->a0_ok = 0;
	src->a0_dmt = 0;
	src->a0_enh = 0;
	src->a2_ok = 0;
	src->a2_status = 0;

	/* THE BANK THE CROSS-CHECKABLE PINS LIVE ON. Independence is a property of the GPIO
	 * CONTROLLER those pins were read through, so it must be that controller that is walked.
	 * If the cross-checkable pins are spread across two banks this cannot be answered with a
	 * single adapter number, and the honest reply is UNKNOWN -- which fails closed. */
	for (i = 0; i < cage->npins; i++) {
		if (!cage->pins[i].phase_a_read)
			continue;
		if (!(cage->pins[i].bit & DP_SFF_XCHK_PINS))
			continue;
		if (bank < 0)
			bank = (int)cage->pins[i].bank;
		else if (bank != (int)cage->pins[i].bank)
			return;			/* two banks, one answer: refuse */
	}
	if (bank < 0)
		return;

	chip = sff_chip_for_bank((uint8_t)bank);
	if (!chip)
		return;
	src->gpio_i2c_adapter = sff_gpiochip_i2c_adapter(chip->dir);

	/* A0h bytes 92-93 in one transaction: Diagnostic Monitoring Type and Enhanced Options.
	 * They say whether an A2 page exists at all and which of byte 110's bits the module
	 * actually maintains -- so they are read FIRST and A2 is not trusted without them. */
	if (sff_i2c_read(cage->i2c_bus, cage->i2c_a0, SFF8472_A0_DMT, a0, 2) == 0) {
		src->a0_ok = 1;
		src->a0_dmt = a0[0];
		src->a0_enh = a0[1];
	}
	if (sff_i2c_read(cage->i2c_bus, cage->i2c_a2, SFF8472_A2_STATUS, &src->a2_status, 1) == 0)
		src->a2_ok = 1;
	else
		src->a2_status = 0;	/* consulted only under a2_ok; zeroed so a future reader
					 * cannot pick up a stale byte and call it a reading */
}

#else	/* !__linux__ */

/*
 * NOT LINUX — a build host, where the test suite runs. There is no gpiochip to resolve, so the
 * honest answer is the same one the appliance gives when the label is absent: E_GPIO. This is
 * the direct analogue of dp_swop_test.c running with the SMI window deliberately unmapped.
 *
 * NOTE THE COST, because it is the project's known cfg-split hazard: the Linux half above is
 * never compiled on this host. It must be compiled on a Linux box (mamoru-dev01) before it is
 * believed, and it has been — but a green run HERE says nothing about it.
 */
static const struct sff_chip *sff_chip_for_bank(uint8_t bank)
{
	/* Consult the bank table anyway. Not decoration: it keeps the "an unlisted bank is a
	 * guess we refuse to make" path identical on both halves, so the table cannot rot
	 * untested on the only host the suite actually runs on. */
	(void)dp_sff_label_for_bank(bank);
	/* And hold the SAME cache invariant as the Linux half: a negative resolution is never
	 * cached. There is nothing to release here, which is the point -- the two halves must
	 * not drift into different states for the same outcome.
	 *
	 * (It also keeps this configuration COMPILING. A non-Linux build without DP_SWOP_TEST
	 * otherwise leaves sff_chip_forget() with no caller and -Werror rejects it. dp_fwd is
	 * Linux-only so that build ships nowhere, but a configuration nobody can compile is a
	 * configuration nobody can check -- and this is the SECOND time in this file that the
	 * two halves disagreed about what compiles, in opposite directions.) */
	sff_chip_forget();
	return NULL;
}

static int sff_read_line(const struct sff_chip *chip, uint8_t line, uint8_t driven, int *out)
{
	(void)chip; (void)line; (void)driven; (void)out;
	return -1;
}

/*
 * NOT LINUX -- no i2c adapter and no sysfs parentage to walk, so nothing can be corroborated.
 * The honest answer is the SAME one the appliance gives when the bus does not answer: UNKNOWN
 * parentage and no bytes, which dp_sff_xchk_classify() lands in the THIRD state. It is not an
 * error and it does not clear anybody's `valid` bit.
 *
 * Note what this costs, because it is the project's known cfg-split hazard and it applies here
 * exactly as it does to the GPIO half above: the Linux transport is never compiled on this
 * host. The CLASSIFIER is, and that is where every decision lives -- but the i2c transaction
 * and the parentage walk must be compiled on a Linux box before they are believed.
 */
static void sff_xchk_gather(const struct dp_sff_cage *cage, struct dp_sff_xsrc *src)
{
	(void)cage;
	src->gpio_i2c_adapter = DP_SFF_I2C_ADAPTER_UNKNOWN;
	src->a0_ok = 0;
	src->a0_dmt = 0;
	src->a0_enh = 0;
	src->a2_ok = 0;
	src->a2_status = 0;
}

#endif	/* __linux__ */

static uint8_t sff_read_pins_real(const struct dp_sff_cage *cage, uint8_t *raw, uint8_t *ok)
{
	const struct sff_chip *chip;
	int resolved_any = 0;
	uint8_t i;
	int level;

	*raw = 0;
	*ok = 0;
	if (!cage)
		return DP_SWOP_E_GPIO;		/* caller's bug; never a silent zero */

	for (i = 0; i < cage->npins; i++) {
		if (!cage->pins[i].phase_a_read)
			continue;		/* driven-only: never claimed, never read */
		chip = sff_chip_for_bank(cage->pins[i].bank);
		if (!chip)
			continue;		/* label unresolvable — bit stays out of *ok */
		resolved_any = 1;
		if (sff_read_line(chip, cage->pins[i].line, cage->pins[i].driven, &level) < 0)
			continue;		/* this ONE line failed; the others still count */
		if (level)
			*raw = (uint8_t)(*raw | cage->pins[i].bit);
		*ok = (uint8_t)(*ok | cage->pins[i].bit);
	}

	/* E_GPIO is for "the controller is not there", which is a whole-cage fact. A line that
	 * failed while the chip resolved is reported by its CLEARED bit in *ok, so the reply can
	 * still carry the lines that did read — the contract's "a single line that fails clears
	 * its valid bit instead". */
	if (!resolved_any) {
		*raw = 0;
		*ok = 0;
		return DP_SWOP_E_GPIO;
	}
	return DP_SWOP_OK;
}

/*
 * THE TEST SEAM, and it is placed so there is exactly ONE symbol a caller can reach.
 *
 * dp_swop.c's reg_read seam had a defect worth not repeating: one call site — OP_READ's, the op
 * a hardware control depends on — called the real function directly instead of the macro, so
 * substituting a stub had no effect on it and its whole success path went untested while the
 * suite stayed green. Here the real implementation is `static` and NAMELESS to the outside; the
 * only exported entry is this one, so a call that bypasses the hook cannot be written.
 *
 * Absent DP_SWOP_TEST there is no function pointer in the binary and no hook to divert: this
 * collapses to a tail call to a static function. build_fwd.sh never defines DP_SWOP_TEST.
 */
#ifdef DP_SWOP_TEST
static uint8_t (*sff_read_hook)(const struct dp_sff_cage *, uint8_t *, uint8_t *);

void dp_sff_test_set_read(uint8_t (*fn)(const struct dp_sff_cage *cage, uint8_t *raw,
					uint8_t *ok))
{
	sff_read_hook = fn;
}

void dp_sff_test_set_chip_label(const char *label)
{
	sff_label_override = label;
	sff_chip_forget();	/* a cached resolution belongs to the OLD label */
}

static const struct dp_sff_xsrc *sff_xsrc_override;

void dp_sff_test_set_xsrc(const struct dp_sff_xsrc *src)
{
	sff_xsrc_override = src;
}
#endif

uint8_t dp_sff_read_pins(const struct dp_sff_cage *cage, uint8_t *raw, uint8_t *ok)
{
#ifdef DP_SWOP_TEST
	if (sff_read_hook)
		return sff_read_hook(cage, raw, ok);
#endif
	return sff_read_pins_real(cage, raw, ok);
}

/*
 * THE CROSS-CHECK'S ONE PUBLIC ENTRY, and the seam is placed the same way dp_sff_read_pins()'s
 * is: the TRANSPORT is what a test may displace, never the classifier. So a suite that
 * constructs an i2c-expander board still runs the shipping three-state logic, and there is no
 * way to write a call that bypasses it.
 */
void dp_sff_cross_check(const struct dp_sff_cage *cage, uint8_t logical, uint8_t want,
			struct dp_sff_xchk *out)
{
	struct dp_sff_xsrc src;

	out->agreed = 0;
	out->disagreed = 0;
	out->nocheck = (uint8_t)(want & DP_SFF_XCHK_PINS);
	if (!cage || !out->nocheck)
		return;			/* nothing asked: no bus transaction, no verdict */

#ifdef DP_SWOP_TEST
	if (sff_xsrc_override) {
		dp_sff_xchk_classify(sff_xsrc_override, (int)cage->i2c_bus, logical, want, out);
		return;
	}
#endif
	sff_xchk_gather(cage, &src);
	dp_sff_xchk_classify(&src, (int)cage->i2c_bus, logical, want, out);
}

#endif /* DP_SFF_C */
