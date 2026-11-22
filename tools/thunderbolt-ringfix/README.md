# thunderbolt-ringfix: two fixes for the in-tree `thunderbolt` driver

Until these fixes are in mainline, Strix Halo hosts should run a rebuilt
`thunderbolt.ko` with them. `build-dkms.sh` makes that a DKMS package for the
running kernel.

| Patch | What it fixes |
| --- | --- |
| `0001-thunderbolt-Do-not-match-XDomain-responses` | `tb_xdomain_match()` can match a received packet to a fire-and-forget XDomain response, whose request is then released twice (`refcount_t: underflow` in `tb_cfg_request_work`, a use-after-free). |
| `0002-thunderbolt-Flush-the-ring-interrupt-status-clear` | On AMD NHIs `ring_msix()` clears the ring's interrupt status with a posted write that can land after the next completion and erase it; that frame then waits for some later interrupt: ~75 ms stalls (one QP ACK timeout) and native-mode retransmits. Reads the register back after the clear. |
| `0003-thunderbolt-Clear-the-ring-interrupt-status-in-ring_work` | Makes 0002 cheap: for rings without `start_poll` the status is cleared in `ring_work()`, after the frames already completed are handed out, followed by the read-back and a second scan. One read per batch instead of per interrupt, and about half the interrupts. |

Measured on two AXB35-02 hosts with USB4 H2H on (Fedora 7.3.4, November
2026), against the stock module:

| | stock | ringfix |
| --- | ---: | ---: |
| 16 KiB ping-pong, exchanges over 10 ms | 50–78 per 1000 | 0 of 3000 |
| native `ib_write_bw` 4 MiB | 54 Gb/s | 70–73 |
| native bidirectional 4 MiB | 97–101 Gb/s | 113–122 |
| DMA-BUF `ib_write_bw` 4 MiB / 64 B latency | 61 Gb/s / 28–30 µs | 61 / 27.6–28.2 µs |
| `thunderbolt_net` iperf3 per cable | ~31 Gb/s | ~31 |

0002 alone fixes the stalls but reads the register back in every interrupt,
which cost native mode 10–30% (about a million interrupts per second, ~1.5 µs
each); 0003 removes that cost. Upstream, 0001 and 0002 are the bug fixes and
0003 a follow-up. Details: [bench/results/2026-11-22-usb4-h2h.md](../../bench/results/2026-11-22-usb4-h2h.md).

## Install

```sh
sudo tools/thunderbolt-ringfix/build-dkms.sh            # running kernel
sudo tools/thunderbolt-ringfix/build-dkms.sh 7.3.4-100.fc43.x86_64
```

It downloads `linux-<version>.tar.xz` from kernel.org, takes
`drivers/thunderbolt`, applies `patches/*.patch` and installs DKMS package
`thunderbolt-ringfix/<version>.2` (fix revision 2 = with 0003), built only for kernels of that version
(not `+debug`). DKMS installs it to `extra/` and moves the stock module aside
("Original modules exist"); the distribution's hooks copy it into the
initramfs. Reboot, then check `modinfo -n thunderbolt` ends in
`extra/thunderbolt.ko.xz`.

- A patch that is already in the kernel is skipped. If one no longer
  applies, the script stops; that kernel needs a new version of the patch.
- Distributions that patch `drivers/thunderbolt` (Fedora 7.3.4 does not) need
  their own sources: `--src /path/to/drivers/thunderbolt`. On Fedora:
  `dnf download --source kernel-<version>` and extract that directory from the
  `linux-*.tar.xz` inside, after applying the SRPM's patches if any touch it.
- Each new kernel version needs the script run again; until then that kernel
  uses its stock module.
- `--out DIR` only writes the package source to `DIR`.

Undo: `sudo dkms remove thunderbolt-ringfix/<version>.2 --all`, reboot.
