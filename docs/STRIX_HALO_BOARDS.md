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
  Ubuntu's 7.2.6, Fedora's 7.2.11 and Fedora's 7.3.3 and 7.3.4. On other kernels
  (including 7.4 until its layout has been checked) the helper is compiled out
  and loading with `dmabuf_batch_doorbell=1` fails with `EOPNOTSUPP`; leave it
  off there.
- **In-tree `thunderbolt` driver.** Two driver bugs affect this setup (both
  verified on 7.3.3 and 7.3.4, the XDomain one also seen on 7.2.x). Their fixes are not
  yet in mainline, so the AXB35 hosts run a rebuilt `thunderbolt.ko` with both
  patches ([tools/thunderbolt-ringfix](../tools/thunderbolt-ringfix/README.md)). On these AMD NHIs `ring_msix()` clears
  a ring's interrupt status with a posted write that can land after the next
  completion and erase it: the frame then waits for some later interrupt,
  which shows up as ~75 ms stalls (often exactly one QP ACK timeout) and, in
  native mode, as retransmits. The fix (v3, November 2026) leaves the status
  set in `ring_msix()` and clears it in `ring_work()`: it hands out the frames
  already completed, clears, reads the register back and scans once more. It
  removes the stalls and halves the interrupt rate; native mode got about 30%
  faster with it (a first version that read back in every interrupt cost
  native mode 10–30%). Separately, `tb_xdomain_match()` can match a fire-and-forget XDomain
  response, so its request is freed twice (`refcount_t: underflow` in
  `tb_cfg_request_work`, a use-after-free). The stock driver runs this
  module, but with those stalls and that use-after-free.
- **USB4 host-to-host setting (H2H).** BIOSes before 1.10 on the AXB35-02
  leave AMD's "USB4 H2H Enhancement" off, which caps every DMA path at about
  10 Gb/s. [tools/usb4-h2h](../tools/usb4-h2h/README.md) turns it on at boot.
- **Secure Boot.** The Fedora hosts run lockdown; DKMS signs the module with
  the enrolled MOK. Writable debugfs and dynamic debug control are then
  unavailable, so use module parameters (`dyndbg=...`) instead.

Both tested setups boot with `amd_iommu=off amdgpu.gttsize=126976
ttm.pages_limit=32505856`. IOMMU-enabled operation is not qualified.

## Options

All Strix Halo optimizations are compiled on by default, so `/etc/modprobe.d/thunderbolt-ibverbs.conf` only needs:

```text
blacklist thunderbolt_net
options thunderbolt_ibverbs roce_netdev=tbv0
```

That enables `dmabuf_verbs=1`, `lanes=2`, `dmabuf_batch_doorbell=auto` (128-frame burst on AMD `158d/158e`), `dmabuf_push_done=1`, `dmabuf_credits=1`, `dmabuf_coalesce_copy=1`, `dmabuf_reuse_buffers=1`, `dmabuf_host_direct=1`, `dmabuf_host_credits=1`, and `dmabuf_host_direct_min_bytes=4096`.

To override individual tuning knobs (for example `dmabuf_poll_us=25 dmabuf_poll_control=1` on the Framework Desktop), build with `TBV_DEV_PARAMS=1`:

| Option (`TBV_DEV_PARAMS=1`) | Default | Effect |
|---|---|---|
| `dmabuf_batch_doorbell` | `auto` | Publish a group of data-ring descriptors with one MMIO doorbell write on AMD `158d/158e` NHIs; falls back to `tb_ring_tx/rx()` elsewhere |
| `dmabuf_post_burst` | `0` (128 with bulk doorbell) | Frames posted per rail before moving to the sibling rail (1–512) |
| `dmabuf_coalesce_copy` | `1` | Copy each host-staged TX lane once instead of per 4 KiB frame |
| `dmabuf_reuse_buffers` | `1` | Keep one drained descriptor/staging batch per rail and direction (at most 2 MiB staging each) |
| `dmabuf_host_direct` | `1` | Map page-aligned pinned host MRs straight to each NHI instead of staging them |
| `dmabuf_host_direct_min_bytes` | `4096` | Smallest direct host transfer (4096–65536) |
| `dmabuf_host_credits` | `1` | Pre-arm SEND credits for direct host MRs too, not only GPU MRs |
| `dmabuf_poll_us` | `0` | Fallback timer that runs the ordinary ring workers every N µs while batches are in flight (10–10000, 0 off) |
| `dmabuf_poll_control` | `0` | Keep that timer running while any QP exists and also progress the XDomain control rings |

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
