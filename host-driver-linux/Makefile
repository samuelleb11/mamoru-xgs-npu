# SPDX-License-Identifier: GPL-2.0 OR MIT
# Out-of-tree build for the Marvell AGNIC host driver.
#
# Build against a kernel tree matching your target's running kernel:
#   make                                  # uses the running kernel's build tree
#   make KDIR=/path/to/kernel-build-tree  # explicit (e.g. cross / a specific tree)
# Produces mamoru_agnic.ko; load with: sudo insmod mamoru_agnic.ko

obj-m += mamoru_agnic.o
mamoru_agnic-y := agnic_main.o agnic_mgmt.o agnic_pcinet.o agnic_txrx.o agnic_pport.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
