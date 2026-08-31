// SPDX-License-Identifier: MIT
/* dp_swcfg.c - deterministic Marvell 88E6193X (Amethyst) switch config tool.
 *
 * Replaces the fragile interactive UMSD_MCLI (libcli / needs a TTY, hangs over
 * an ssh pipe) so the NPU data-plane's 8-port separation can be driven as code:
 * no TTY, no heredocs, exact exit codes.
 *
 * Reuses the umsd host layer:
 *   qdInit(baseAddr, bus_interface)  -> from host/src/init.c  (registers the
 *       SMIRead/SMIWrite BSP callbacks that talk to /dev/mvmdio-uio via
 *       libMRegAccess_mvmdio_uio) and leaves the loaded device in `qddev`.
 *   qddev->SwitchDevObj.PORTCTRLObj.*  -> the per-device (Amethyst) driver ops.
 *
 * Link: dp_swcfg = this + init.o + libMsdDrv.a + libMRegAccess_mvmdio_uio.a
 *
 * Usage (on the NPU):
 *   dp_swcfg dump                     read frameMode/link/speed/vlan for all ports
 *   dp_swcfg framemode <port> <mode>  mode: 0=NORMAL 1=DSA 2=PROVIDER 3=ETHER_TYPE_DSA
 *
 * SMI addressing matches /usr/marvell/umsd_cn91xx.cfg: baseAddr 6, SMI Multi-Chip (1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msdApi.h"

extern MSD_QD_DEV *qddev;
extern MSD_STATUS  qdInit(int baseAddr, int bus_interface);

/* MDIO access layer (libMRegAccess_mvmdio_uio): must open /dev/mvmdio-uio and
 * select the SMI bus before any register access, else readRegister() does
 * read(fd=0,...) and blocks forever on stdin. */
extern int  smiOpen(void);
extern void smiClose(void);
extern void setMDIOBusId(int bus_id);

#define SW_BASEADDR 6   /* SMI device address of the 88E6193X            */
#define SW_IFACE    1   /* SMI Multi-Chip interface (indirect access)     */
#define SW_BUSID    0   /* MDIO bus id (mvmdio-uio)                       */

static const char *fmname(int m)
{
    switch (m) {
    case 0: return "NORMAL";
    case 1: return "DSA";
    case 2: return "PROVIDER";
    case 3: return "ETHER_TYPE_DSA";
    default: return "?";
    }
}

static void dump(void)
{
    int p, nports = qddev->numOfPorts;

    printf("deviceId=%#x  ports=%d  baseAddr=%#x  rev=%#x\n",
           (unsigned)qddev->deviceId, nports,
           qddev->baseRegAddr, qddev->revision);
    printf("%-4s %-16s %-6s %-6s %s\n", "port", "frameMode", "link", "speed", "vlanMembers");

    for (p = 0; p < nports; p++) {
        MSD_FRAME_MODE fm = 0;
        MSD_BOOL       link = 0;
        MSD_PORT_SPEED spd = 0;
        MSD_LPORT      mem[32];
        MSD_U8         mlen = 0;
        char           fmbuf[24], linkbuf[8], spdbuf[12], vbuf[128];
        int            i, n;

        if (qddev->SwitchDevObj.PORTCTRLObj.gprtGetFrameMode &&
            qddev->SwitchDevObj.PORTCTRLObj.gprtGetFrameMode(qddev, (MSD_LPORT)p, &fm) == MSD_OK)
            snprintf(fmbuf, sizeof fmbuf, "%d:%s", (int)fm, fmname((int)fm));
        else
            strcpy(fmbuf, "-");

        if (qddev->SwitchDevObj.PORTCTRLObj.gprtGetLinkState &&
            qddev->SwitchDevObj.PORTCTRLObj.gprtGetLinkState(qddev, (MSD_LPORT)p, &link) == MSD_OK)
            strcpy(linkbuf, link ? "UP" : "down");
        else
            strcpy(linkbuf, "-");

        if (qddev->SwitchDevObj.PORTCTRLObj.gprtGetSpeed &&
            qddev->SwitchDevObj.PORTCTRLObj.gprtGetSpeed(qddev, (MSD_LPORT)p, &spd) == MSD_OK)
            snprintf(spdbuf, sizeof spdbuf, "%d", (int)spd);
        else
            strcpy(spdbuf, "-");

        n = 0;
        vbuf[0] = '\0';
        if (qddev->SwitchDevObj.PORTCTRLObj.gprtGetVlanPorts &&
            qddev->SwitchDevObj.PORTCTRLObj.gprtGetVlanPorts(qddev, (MSD_LPORT)p, mem, &mlen) == MSD_OK) {
            for (i = 0; i < (int)mlen && i < 32; i++)
                n += snprintf(vbuf + n, sizeof vbuf - n, "%s%d", i ? "," : "", (int)mem[i]);
        } else {
            strcpy(vbuf, "-");
        }

        printf("%-4d %-16s %-6s %-6s %s\n", p, fmbuf, linkbuf, spdbuf, vbuf);
    }
}

int main(int argc, char **argv)
{
    setMDIOBusId(SW_BUSID);
    if (smiOpen() != 0) {
        fprintf(stderr, "dp_swcfg: smiOpen(/dev/mvmdio-uio) failed\n");
        return 1;
    }
    if (qdInit(SW_BASEADDR, SW_IFACE) != MSD_OK) {
        fprintf(stderr, "dp_swcfg: qdInit(%d,%d) failed\n", SW_BASEADDR, SW_IFACE);
        return 1;
    }
    if (!qddev) {
        fprintf(stderr, "dp_swcfg: no device loaded\n");
        return 1;
    }

    if (argc >= 4 && strcmp(argv[1], "framemode") == 0) {
        int port = atoi(argv[2]);
        int mode = atoi(argv[3]);
        MSD_STATUS st;
        if (!qddev->SwitchDevObj.PORTCTRLObj.gprtSetFrameMode) {
            fprintf(stderr, "dp_swcfg: gprtSetFrameMode not supported by this device\n");
            return 1;
        }
        st = qddev->SwitchDevObj.PORTCTRLObj.gprtSetFrameMode(qddev, (MSD_LPORT)port,
                                                              (MSD_FRAME_MODE)mode);
        printf("setFrameMode port %d -> %d:%s : %s\n",
               port, mode, fmname(mode), st == MSD_OK ? "OK" : "FAIL");
        return st == MSD_OK ? 0 : 1;
    }

    /* default action: dump */
    dump();
    return 0;
}
