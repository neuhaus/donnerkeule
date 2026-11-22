// SPDX-License-Identifier: GPL-2.0
/*
 * bos_usb4h2h: apply AMD's "USB4 H2H Enhancement" at runtime on Strix Halo boards whose BIOS predates it.
 *
 * BIOS 1.10 (AGESA StrixHaloPI 1.0.0.2e, FchHNUsb4H2HEnhanceEnable) sends SMU message 0x2C (USB config update)
 * once per USB4 router: args {0xD44EC, mask 0xC000, value 0xC000 or 0, router instance 0x1500003 / 0x1700003}.
 * The SMU is reached through the BIOS mailbox on SMN (msg 0x3B10928, response 0x3B10978, args 0x3B10998..AC),
 * using the host bridge's SMN index/data pair at PCI config 0xB8/0xBC, exactly as the BIOS does.
 *
 * Parameters: enable=1 (0 writes 0, the BIOS's "Disabled"; -1 leaves H2H alone), test_only=1 sends only the SMU test
 * message. stapm_mw / fast_mw / slow_mw (0 = leave alone) set the power limits with the SMU messages ryzenadj uses on
 * Strix Halo (0x14 STAPM, 0x15 fast PPT, 0x16 slow PPT; milliwatts), the same as the BIOS power mode's limits.
 *
 * Copyright 2026 the StrixFold contributors.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mutex.h>

static int enable = 1;
module_param(enable, int, 0444);
static int test_only;
module_param(test_only, int, 0444);
static uint stapm_mw, fast_mw, slow_mw;
module_param(stapm_mw, uint, 0444);
module_param(fast_mw, uint, 0444);
module_param(slow_mw, uint, 0444);

#define SMN_INDEX 0xB8
#define SMN_DATA 0xBC
#define MBOX_MSG 0x3B10928
#define MBOX_RSP 0x3B10978
#define MBOX_ARG0 0x3B10998
#define MSG_TEST 0x01
#define MSG_USB_CONFIG_UPDATE 0x2C
#define MSG_SET_STAPM_LIMIT 0x14
#define MSG_SET_FAST_LIMIT 0x15
#define MSG_SET_SLOW_LIMIT 0x16

static struct pci_dev *root;
static DEFINE_MUTEX(smn_lock);

static u32 smn_read(u32 addr)
{
	u32 v;

	pci_write_config_dword(root, SMN_INDEX, addr);
	pci_read_config_dword(root, SMN_DATA, &v);
	return v;
}

static void smn_write(u32 addr, u32 v)
{
	pci_write_config_dword(root, SMN_INDEX, addr);
	pci_write_config_dword(root, SMN_DATA, v);
}

static int wait_rsp(u32 *rsp)
{
	int i;

	for (i = 0; i < 2000; i++) {
		*rsp = smn_read(MBOX_RSP);
		if (*rsp)
			return 0;
		usleep_range(500, 1000);
	}
	return -ETIMEDOUT;
}

/* send one BIOS-mailbox message; args[] in, args[] out; returns the SMU response code or a negative error */
static int smu_send(u32 msg, u32 args[6])
{
	u32 rsp;
	int i, ret;

	mutex_lock(&smn_lock);
	ret = wait_rsp(&rsp);
	if (ret) {
		pr_err("bos_usb4h2h: mailbox busy before message %#x (response register stays 0)\n", msg);
		goto out;
	}
	smn_write(MBOX_RSP, 0);
	for (i = 0; i < 6; i++)
		smn_write(MBOX_ARG0 + 4 * i, args[i]);
	smn_write(MBOX_MSG, msg);
	ret = wait_rsp(&rsp);
	if (ret) {
		pr_err("bos_usb4h2h: no response to message %#x\n", msg);
		goto out;
	}
	for (i = 0; i < 6; i++)
		args[i] = smn_read(MBOX_ARG0 + 4 * i);
	ret = rsp;
out:
	mutex_unlock(&smn_lock);
	return ret;
}

static int __init bos_usb4h2h_init(void)
{
	static const u32 routers[] = { 0x1500003, 0x1700003 };
	u32 args[6];
	int i, rsp;

	root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!root || root->vendor != PCI_VENDOR_ID_AMD)
		return -ENODEV;

	memset(args, 0, sizeof(args));
	args[0] = 0x1234;
	rsp = smu_send(MSG_TEST, args);
	pr_info("bos_usb4h2h: SMU test message: response %#x, arg0 0x1234 -> %#x (expect 0x1235)\n", rsp, args[0]);
	if (rsp != 1 || args[0] != 0x1235) {
		pr_err("bos_usb4h2h: the mailbox did not answer as expected; nothing else sent\n");
		pci_dev_put(root);
		return -EIO;
	}
	if (test_only)
		goto done;

	{
		static const struct { u32 msg; const char *name; } lim[] = {
			{ MSG_SET_STAPM_LIMIT, "STAPM" }, { MSG_SET_FAST_LIMIT, "fast PPT" }, { MSG_SET_SLOW_LIMIT, "slow PPT" } };
		uint want[] = { stapm_mw, fast_mw, slow_mw };

		for (i = 0; i < ARRAY_SIZE(lim); i++) {
			if (!want[i])
				continue;
			if (want[i] < 15000 || want[i] > 150000) {
				pr_err("bos_usb4h2h: %s limit %u mW is outside 15000-150000; not sent\n", lim[i].name, want[i]);
				continue;
			}
			memset(args, 0, sizeof(args));
			args[0] = want[i];
			rsp = smu_send(lim[i].msg, args);
			pr_info("bos_usb4h2h: set %s limit %u mW -> response %#x\n", lim[i].name, want[i], rsp);
		}
	}
	if (enable < 0)
		goto done;

	for (i = 0; i < ARRAY_SIZE(routers); i++) {
		memset(args, 0, sizeof(args));
		args[0] = 0xD44EC;
		args[1] = 0xC000;
		args[2] = enable ? 0xC000 : 0;
		args[3] = routers[i];
		rsp = smu_send(MSG_USB_CONFIG_UPDATE, args);
		pr_info("bos_usb4h2h: router %#x: USB config update 0xD44EC mask 0xC000 value %#x -> response %#x, args back %08x %08x %08x %08x\n",
			routers[i], enable ? 0xC000 : 0, rsp, args[0], args[1], args[2], args[3]);
	}
done:
	pci_dev_put(root);
	return 0;
}

static void __exit bos_usb4h2h_exit(void)
{
}

module_init(bos_usb4h2h_init);
module_exit(bos_usb4h2h_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Runtime USB4 H2H Enhancement for Strix Halo (SMU message 0x2C, as BIOS 1.10)");
