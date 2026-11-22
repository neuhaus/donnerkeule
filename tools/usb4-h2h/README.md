# usb4-h2h: AMD's USB4 host-to-host setting on Strix Halo

Optional and advanced. Bosgame's BIOS 1.10 for the Sixunited AXB35-02 board
(Bosgame M5) turns on an AMD firmware setting, "USB4 H2H Enhancement"
(`FchHNUsb4H2HEnhanceEnable` in AGESA StrixHaloPI 1.0.0.2e), that older BIOSes
leave off and do not show in their menus. `bos_usb4h2h.ko` sends the same
message from Linux, so no BIOS update is needed. The module and the procedure
come from StrixFold's
[`tools/m5-usb4`](https://github.com/neuhaus/strixfold/tree/main/tools/m5-usb4) (also published as
[a gist](https://gist.github.com/georgewhewell/ffd84802a53e39f26d88a8b366c8fe2b)),
GPL-2.0 like this repository. `usb4-h2h.sh` is this repository's version of
their `m5-usb4` script: it finds the NHIs by PCI ID instead of a fixed
address and unloads and restores `thunderbolt_ibverbs` around the reset.

With two USB4 cables at Gen 3 it lifts DMA-BUF verbs mode from about 39 to
61–66 Gb/s one way (85 → 120–130 Gb/s both ways) and a single-path RDMA READ
from 9.8 to about 26 Gb/s; see [Results](#results).

| File | What it is |
| --- | --- |
| `bos_usb4h2h.c`, `Makefile`, `dkms.conf` | Kernel module (unchanged from StrixFold) and its DKMS package. Sends a test message to the SMU, then SMU message `0x2C` (USB config update: register `0xD44EC`, mask/value `0xC000`) once per USB4 router (`0x1500003`, `0x1700003`), through the BIOS mailbox over SMN. |
| `usb4-h2h.sh` | `status`, `apply`, `disable`. `apply` unloads `thunderbolt_ibverbs`, `thunderbolt_net` and `thunderbolt`, loads and unloads the module, resets both NHIs (`1022:158d`, `1022:158e`) through ACPI, then loads the drivers that were loaded before. |
| `usb4-h2h.service` | Runs `usb4-h2h apply` at every boot. |

## The risk

The module talks to the system management unit (SMU) through the mailbox the
BIOS uses. It sends a test message first and stops if the answer is not the
expected one, but a wrong message to the SMU can hang the machine until a power
cycle. The router instance numbers come from the AXB35-02's BIOS. The script
refuses other boards unless `USB4_H2H_ANY_BOARD=1` is set, and on other boards
they are unverified. A reboot clears the setting; nothing is written to flash.

## Install (DKMS + boot service)

On **both** hosts, with Secure Boot using the DKMS MOK (see the main README),
from the repository root:

```sh
sudo cp -r tools/usb4-h2h /usr/src/usb4-h2h-1.0
sudo dkms install usb4-h2h/1.0          # builds and signs for the running kernel
sudo install -m 755 tools/usb4-h2h/usb4-h2h.sh /usr/local/sbin/usb4-h2h
sudo install -m 644 tools/usb4-h2h/usb4-h2h.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Harmless first check (only the SMU's echo message):

```sh
sudo modprobe bos_usb4h2h test_only=1 && sudo dmesg | grep bos_usb4h2h | tail -1 && sudo rmmod bos_usb4h2h
# bos_usb4h2h: SMU test message: response 0x1, arg0 0x1234 -> 0x1235 (expect 0x1235)
```

Any other answer: stop here. Otherwise apply it once by hand (stop anything
using the cables first: they drop for about 10–20 s), then enable it at boot:

```sh
sudo usb4-h2h apply
sudo systemctl enable usb4-h2h.service
```

`apply` prints the module's lines (both routers must answer `response 0x1`),
the two resets and the link speeds afterwards. Check that the links are still
at `20.0 Gb/s x2` (Gen 3); H2H does not fix a Gen 2 link (see
[Checking USB4 Link Speed](../../README.md#checking-usb4-link-speed-and-diagnostics)).
The SMU message cannot be read back, so `journalctl -b -u usb4-h2h` is the
record of whether it was applied this boot. `disable` sends the BIOS's
"Disabled" value the same way. To build without DKMS, `make -C
/lib/modules/$(uname -r)/build M=$PWD modules`, sign the `.ko` and pass its
path: `sudo ./usb4-h2h.sh apply ./bos_usb4h2h.ko`.

Undo: `sudo systemctl disable usb4-h2h.service`, reboot;
`sudo dkms remove usb4-h2h/1.0 --all` removes the module.

## What the two bits do

Register `0xD44EC`, measured 22 November 2026 on fuzzy/misty (AXB35-02,
BIOS 1.07), each value written to both routers followed by a router reset:

| Bits set | DMA-BUF WRITE 4 MiB | DMA-BUF bidir 4 MiB | native READ 1 MiB |
| --- | ---: | ---: | ---: |
| none (BIOS default) | 39.0 Gb/s | 84.9 | 9.9 |
| 15 | 39.2 | 80.9 | 9.7 |
| 14 | 45.1 | 97.3 | 17.3 |
| 14 + 15 (H2H) | 60.6 | 119.5 | 19.7 |
| 13 + 14 + 15 | 64.4 | 120.0 | 19.8 |
| 14 + 15 + 16 | 60.3 | 119.6 | 19.5 |

Bit 14 lifts a cap of about 10 Gb/s per DMA path (a single-path READ nearly
doubles); bit 15 only matters together with bit 14. The neighbouring bits 13
and 16 made no clear difference (bit 13's one-way +6% was across a reboot).
These runs used a patched `thunderbolt.ko` that is slower in native mode, so
compare within the table, not with the sweep below.

## Results

[bench/results/2026-11-22-usb4-h2h.md](../../bench/results/2026-11-22-usb4-h2h.md).
