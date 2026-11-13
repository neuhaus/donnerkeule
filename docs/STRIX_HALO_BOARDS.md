# Strix Halo boards: Sixunited AXB35 and Framework Desktop

One module build and one set of options for two-cable TP=2 between two hosts
on either board:

- **Sixunited AXB35** (Bosgame M5, GMKtec EVO-X2, FEVM FA-EX9 and others),
  tested as fuzzy/misty: AXB35-02, Fedora 43, kernel 7.2.11 and since
  November 2026 7.3.3.
- **Framework Desktop** mainboard, tested by the
  [donnerkeule fork of lfthht](https://github.com/lfthht/donnerkeule/tree/codex/framework-desktop):
  FRANMFCP06 A6, BIOS 03.04, Ubuntu 26.04, kernel 7.2.6.

## What is the same and what differs

To the driver both boards look the same. Each has the two Strix Halo USB4
host routers `1022:158d` (domain 0) and `1022:158e` (domain 1), each cable
trains to 2 × 20 Gb/s, and each router gives one control and two data ring
pairs, so `lanes=2` yields four rails over two cables. Neither the fork nor
this tree contains code that depends on the board.

What differs is outside the driver:

- **Ports.** On the AXB35 each cable sits on port 2 of its router behind a
  retimer (`0-0:2.1`, `0-0:2.2` in `/sys/bus/thunderbolt/devices`). Which
  physical socket belongs to which domain differs between boards, and domain
  numbers need not match between the two hosts. With both cables connected and
  the default `native_domain_mask`, this does not matter. For a one-cable test
  match the physical link at both ends by its XDomain UUIDs, as described in
  the fork's documentation.
- **Kernel and distribution.** The batched doorbell (below) writes NHI ring
  descriptors directly and needs the `struct tb_ring` layout of Linux 7.2.x or
  7.3.x on x86_64. That layout is checked at build time; it is identical in
  Ubuntu's 7.2.6, Fedora's 7.2.11 and Fedora's 7.3.3. On other kernels
  (including 7.4 until its layout has been checked) the helper is compiled out
  and loading with `dmabuf_batch_doorbell=1` fails with `EOPNOTSUPP`; leave it
  off there.
- **In-tree `thunderbolt` driver.** Two driver bugs affect this setup (both
  verified on 7.3.3, the XDomain one also seen on 7.2.x). Their fixes are not
  yet in mainline, so the AXB35 hosts run a rebuilt `thunderbolt.ko` with both
  patches. On these AMD NHIs `ring_msix()` clears
  a ring's interrupt status with a posted write that can land after the next
  completion and erase it: the frame then waits for some later interrupt,
  which shows up as ~75 ms stalls (often exactly one QP ACK timeout) and, in
  native mode, as retransmits. The fix reads the register back after the
  clear. Separately, `tb_xdomain_match()` can match a fire-and-forget XDomain
  response, so its request is freed twice (`refcount_t: underflow` in
  `tb_cfg_request_work`, a use-after-free). The stock driver runs this
  module, but with those stalls and that use-after-free.
- **Secure Boot.** The Fedora hosts run lockdown; DKMS signs the module with
  the enrolled MOK. Writable debugfs and dynamic debug control are then
  unavailable, so use module parameters (`dyndbg=...`) instead.

Both tested setups boot with `amd_iommu=off amdgpu.gttsize=126976
ttm.pages_limit=32505856`. IOMMU-enabled operation is not qualified.

## Options

These come on top of the two-cable DMA-BUF verbs options (`profile=linux_perf
tbnet=prefer_rdma lanes=2 register_verbs=1 native_write_striping=1
dmabuf_verbs=1 dmabuf_push_done=1 dmabuf_credits=1 roce_netdev=...`):

```text
dmabuf_batch_doorbell=1 dmabuf_post_burst=128 dmabuf_coalesce_copy=1
dmabuf_reuse_buffers=1 dmabuf_host_direct=1 dmabuf_host_credits=1
dmabuf_host_direct_min_bytes=4096
```

On the Framework Desktop the fork also qualified the completion fallback
timer, `dmabuf_poll_us=25 dmabuf_poll_control=1`. On the AXB35 it passes the
same checks and raises host-memory bandwidth slightly (43 vs 39 Gb/s), but did
not improve Gufo and had the larger spread at 2 concurrent requests, so the
AXB35 hosts run without it. All options default to off (or the earlier
behavior), so a module loaded without them behaves like before.

| Option | Effect |
|---|---|
| `dmabuf_batch_doorbell=1` | Publish a group of data-ring descriptors with one MMIO doorbell write, on AMD 158d/158e NHIs only; other rings fall back to `tb_ring_tx/rx()` |
| `dmabuf_post_burst=N` | Frames posted per rail before moving to the sibling rail (1–512, default 16) |
| `dmabuf_coalesce_copy=1` | Copy each host-staged TX lane once instead of per 4 KiB frame |
| `dmabuf_reuse_buffers=1` | Keep one drained descriptor/staging batch per rail and direction (at most 2 MiB staging each) |
| `dmabuf_host_direct=1` | Map page-aligned pinned host MRs straight to each NHI instead of staging them |
| `dmabuf_host_direct_min_bytes=N` | Smallest direct host transfer (4096–65536, default 65536) |
| `dmabuf_host_credits=1` | Pre-arm SEND credits for direct host MRs too, not only GPU MRs |
| `dmabuf_poll_us=N` | Fallback timer that runs the ordinary ring workers every N µs while batches are in flight (10–10000, 0 off) |
| `dmabuf_poll_control=1` | Keep that timer running while any QP exists and also progress the XDomain control rings |

Normal QP close now drains accepted work for up to 2 s before cancelling, and
DMA-BUF mode no longer advertises MAD/CM, so the per-rail `Couldn't create
ib_mad QP1` / `Couldn't open port 1` messages are gone.

## Checking a setup

```sh
ibv_devices                                     # four usb4_rdmaN rails per peer pair
sudo grep -c "descriptor_ready=1 quarantine_reason=0" /sys/kernel/debug/thunderbolt_ibverbs/peers
sudo grep -E "dmabuf_(batch_doorbell|bulk_calls|bulk_fallback|credit_mismatch|progress_skipped)" \
  /sys/kernel/debug/thunderbolt_ibverbs/summary
```

Every rail must be `descriptor_ready=1 quarantine_reason=0`. While traffic
runs, `dmabuf_bulk_calls` grows and `dmabuf_bulk_fallback` stays near zero
when the doorbell path is active. A stopped rail is logged as `rail ... stopped
after a data error or abort (reason N)`; it needs the connection recreated.

## Measurements

On the AXB35 see
[the October 25 report](../bench/results/2026-10-25-donnerkeule-merge.md). On the
Framework Desktop see the fork's
[`docs/framework-desktop.md`](https://github.com/lfthht/donnerkeule/blob/codex/framework-desktop/docs/framework-desktop.md).
