# SPDX-License-Identifier: BSD-2-Clause
# FreeBSD kmod build for if_agnic (Marvell AGNIC / Armada GIU-NIC, CN9130 EP).
#
# Build on a FreeBSD 15.1 host (matching OPNsense 26.7) with kernel sources:
#     make            # -> if_agnic.ko
#     make clean
# Then transfer if_agnic.ko to the XGS box and:  kldload ./if_agnic.ko

KMOD=	if_agnic
SRCS=	if_agnic.c
SRCS+=	agnic_mgmt.c		# Phase 3a: mgmt cmd/notif rings + command round-trip
SRCS+=	agnic_txrx.c		# Phase 3b: ingress (RX) datapath + ifnet
SRCS+=	agnic_nwa.c		# Phase 4a: NW_AGENT mailbox (front-panel port control)
SRCS+=	agnic_pcinet.c		# Phase 5: mvmgmt0 management link to the NPU
SRCS+=	agnic_pport.c		# Phase 4b: per-front-panel-port demux (port1..port9)
# generated interface headers required by the newbus/PCI methods:
SRCS+=	device_if.h bus_if.h pci_if.h

.include <bsd.kmod.mk>
