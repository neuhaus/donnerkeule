# SPDX-License-Identifier: GPL-2.0
#
# Top-level wrapper for out-of-tree builds and DKMS.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
KERNEL_DIR ?= kernel
TBV_LOOPBACK ?= 0
TBV_DEV_PARAMS ?= 0

# Expand only for DKMS targets: plain module builds may omit dkms.conf.
DKMS_PACKAGE_NAME = $(shell awk -F '"' '/^PACKAGE_NAME=/ { print $$2; exit }' dkms.conf)
DKMS_PACKAGE_VERSION = $(shell awk -F '"' '/^PACKAGE_VERSION=/ { print $$2; exit }' dkms.conf)

.PHONY: all clean modules modules_install help dkms-add dkms-build dkms-install dkms-remove dkms-version

all: modules

modules:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) TBV_LOOPBACK=$(TBV_LOOPBACK) TBV_DEV_PARAMS=$(TBV_DEV_PARAMS) modules

modules_install:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) modules_install

clean:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) clean

help:
	$(MAKE) -C $(KERNEL_DIR) help
	@echo ""
	@echo "Top-level targets:"
	@echo "  dkms-add          Register this source tree with DKMS"
	@echo "  dkms-build        Build with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-install      Install with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-remove       Remove this DKMS module version"
	@echo "  dkms-version      Update dkms.conf PACKAGE_VERSION from git describe"

dkms-version:
	@ver=$$(git describe --tags --always --dirty 2>/dev/null | sed 's/^v//' | tr '-' '.'); \
	[ -n "$$ver" ] || { echo "git describe failed" >&2; exit 1; }; \
	sed -i.bak "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$$ver\"/" dkms.conf && rm -f dkms.conf.bak; \
	echo "PACKAGE_VERSION=\"$$ver\""

dkms-add:
	dkms add .

dkms-build:
	dkms build "$(DKMS_PACKAGE_NAME)/$(DKMS_PACKAGE_VERSION)" -k "$(KVER)"

dkms-install:
	dkms install "$(DKMS_PACKAGE_NAME)/$(DKMS_PACKAGE_VERSION)" -k "$(KVER)"

dkms-remove:
	dkms remove "$(DKMS_PACKAGE_NAME)/$(DKMS_PACKAGE_VERSION)" --all
