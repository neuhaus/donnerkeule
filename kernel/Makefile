# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the release-track thunderbolt-ibverbs module.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

obj-m := thunderbolt_ibverbs.o
thunderbolt_ibverbs-y := main.o profile.o configfs.o core.o debugfs.o verbs_dmabuf.o verbs_control.o verbs_cq.o verbs_mr.o ibdev.o link.o native.o trace.o native_control.o native_control_legacy.o native_control_xdomain.o path.o peer.o rail.o service.o ../proto/config.o ../proto/identity.o ../proto/reliability.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -I$(src) -I$(src)/..

# Production may omit the detailed performance tracepoint entirely.
TBV_PROFILING ?= 1
ifeq ($(TBV_PROFILING),0)
ccflags-y += -DTBV_DISABLE_PROFILING
endif

# Thunderbolt dropped the probe ID argument in Linux 7.3. Probe the actual
# headers, including distro backports, instead of assuming a version boundary.
ifneq ($(KERNELRELEASE),)
tbv-probe-without-id := $(call try-run,\
	$(CC) $(NOSTDINC_FLAGS) $(LINUXINCLUDE) $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS) \
	-include $(srctree)/include/linux/compiler_types.h \
	-DMODULE -Werror -c $(src)/compat/tb_service_probe.c -o "$$TMP",y,n)
ifeq ($(tbv-probe-without-id),y)
ccflags-y += -DTBV_TB_SERVICE_PROBE_WITHOUT_ID
endif
tbv-tb-ring-layout := $(call try-run,\
	$(CC) $(NOSTDINC_FLAGS) $(LINUXINCLUDE) $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS) \
	-include $(srctree)/include/linux/compiler_types.h \
	-DMODULE -Werror -c $(src)/compat/tb_ring_layout.c -o "$$TMP",y,n)
ifeq ($(tbv-tb-ring-layout),y)
ccflags-y += -DTBV_TB_RING_LAYOUT
endif
endif

.PHONY: all clean modules modules_install help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) ../proto/*.o ../proto/.*.o.cmd

help:
	@echo "Targets:"
	@echo "  modules            Build thunderbolt_ibverbs.ko"
	@echo "  modules_install    Install under /lib/modules/\$$(uname -r)/extra/"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Build-time parameters:"
	@echo "  TBV_PROFILING=0      Build without detailed performance tracepoints"
	@echo "  KVER=<release>       Build against another installed kernel"
	@echo "  KDIR=<path>          Build against a kernel tree outside /lib/modules"
	@echo ""
	@echo "Module parameters, generated from MODULE_PARM_DESC in the sources:"
	@cat *.c | tr '\n\t' '  ' | grep -oE 'MODULE_PARM_DESC\( *[A-Za-z_0-9]+ *, *("[^"]*" *)+\) *;' | \
		sed -e 's/^MODULE_PARM_DESC([[:space:]]*//' -e 's/)[[:space:]]*;$$//' \
		    -e 's/"[[:space:]]*"//g' -e 's/[[:space:]]*,[[:space:]]*"/ = /' \
		    -e 's/"[[:space:]]*$$//' -e 's/[[:space:]][[:space:]]*/ /g' | \
		sort | sed 's/^/  /'
	@echo ""
	@echo "After a build, 'modinfo thunderbolt_ibverbs.ko' is authoritative."
