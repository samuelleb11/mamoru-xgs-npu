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
#define SPIN_LIMIT		20000

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

/* Wait for the OUTER orion SMI to go idle. */
static int outer_wait(void)
{
	unsigned i;

	for (i = 0; i < SPIN_LIMIT; i++)
		if (!(smi_rd() & OUTER_BUSY))
			return 0;
	return -1;
}

/* Wait for the INNER switch SMI command register to clear its busy bit. */
static int inner_wait(void)
{
	unsigned i;

	for (i = 0; i < SPIN_LIMIT; i++) {
		smi_wr(OUTER_RD_CMD);
		if (outer_wait() < 0)
			return -1;
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

	if (inner_wait() < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_CMD_BASE | INNER_BUSY | INNER_C22 | INNER_OP_READ |
	       ((uint32_t)dev << 5) | reg);
	if (outer_wait() < 0)
		return DP_SWOP_E_BUSY;
	if (inner_wait() < 0)
		return DP_SWOP_E_BUSY;

	smi_wr(OUTER_RD_DATA);
	if (outer_wait() < 0)
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

static uint8_t reg_write(uint8_t dev, uint8_t reg, uint16_t val)
{
	if (!dev_is_legal(dev))
		return DP_SWOP_E_DEV;
	if (reg > 31)
		return DP_SWOP_E_REG;
	if (!g_map)
		return DP_SWOP_E_NOMAP;

	if (inner_wait() < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_DATA_BASE | val);		/* SMI data first  */
	if (outer_wait() < 0)
		return DP_SWOP_E_BUSY;
	smi_wr(OUTER_WR_CMD_BASE | INNER_BUSY | INNER_C22 | INNER_OP_WRITE |
	       ((uint32_t)dev << 5) | reg);		/* then the command */
	if (outer_wait() < 0)
		return DP_SWOP_E_BUSY;
	if (inner_wait() < 0)
		return DP_SWOP_E_BUSY;
	return DP_SWOP_OK;
}

int dp_swop_service(const void *req_buf, unsigned req_len, void *resp_buf, unsigned resp_cap)
{
	struct dp_swop_req req;
	struct dp_swop_resp resp;
	unsigned out_len;
	uint8_t st;

	if (resp_cap < sizeof(resp))
		return -1;			/* cannot even answer; caller's bug */

	memset(&resp, 0, sizeof(resp));
	resp.magic = DP_SWOP_MAGIC;
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
		resp.status = reg_read(req.dev, req.reg, &resp.val);
		break;

	case DP_SWOP_OP_WRITE:
		resp.status = reg_write(req.dev, req.reg, req.val);
		break;

	case DP_SWOP_OP_PORTS: {
		uint8_t d;

		for (d = 0; d < DP_SWOP_PORT_COUNT; d++) {
			st = reg_read(d, DP_SWOP_REG_STATUS, &resp.ports[d].status);
			if (st != DP_SWOP_OK) {
				resp.status = st;
				resp.count = d;	/* how many are actually valid */
				goto out;
			}
			st = reg_read(d, DP_SWOP_REG_VLAN_MAP, &resp.ports[d].vlan_map);
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

	default:
		resp.status = DP_SWOP_E_OP;
		break;
	}

out:
	out_len = sizeof(resp);
	memcpy(resp_buf, &resp, out_len);
	return (int)out_len;
}
