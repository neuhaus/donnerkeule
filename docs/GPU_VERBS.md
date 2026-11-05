# DMA-BUF behind verbs: experimental implementation status

Update from October 16: the [repeated driver check and posting optimization](../bench/results/2026-10-16-verbs-parity.md)
confirms the four RC SEND/WRITE operations with GPU and host MRs.
Alternating posting across the rails of the same NHI brings the tested
1–32 MiB cycles close to the normal host path. Small messages remain slower;
this does not qualify full latency parity or Gufo integration. These
measurements explicitly set `dmabuf_push_done=1`. The defaults remain unchanged.

Addendum: [prepared SEND credits](../bench/results/2026-10-16-verbs-credits.md)
(`dmabuf_credits=1`, off by default) remove PREPARE for GPU SENDs into posted
receive WQEs. In the pattern without a barrier, 32 B and 10,304 B dropped to
about 88–91 µs per verified message; WRITE is unchanged. See
[Prepared SEND credits](#prepared-send-credits).

The new native mode `dmabuf_verbs=1` imports HIP DMA-BUFs through
`ibv_reg_dmabuf_mr()` and uses their NHI DMA addresses behind normal
`ibv_post_send()`, `ibv_post_recv()` and CQ completions. The payload is neither
mapped to the CPU nor copied into a GPU staging buffer.

This is an experimental implementation that is enabled separately.
On October 5, 2026, the first version passed all four operations with direct GPU
MRs on fuzzy and misty, including guard bytes, unsignaled error completions, RNR
and QP/MR recreation. Normal host MRs passed both in the previous mode and on
the new staging path. The performance comparison still shows higher WRITE
latency than the previous verbs path. Results, fixed bugs and logs are in
[2026-10-05-verbs-dmabuf.md](../bench/results/2026-10-05-verbs-dmabuf.md).
The subsequent [optimization round](../bench/results/2026-10-05-verbs-dmabuf-optimization.md)
indexes DMA addresses, cleans up RX on DMA completion and hands large descriptor
packets to different NHIs in parallel. At 1 and 20 MiB, latency within the
direct path dropped by about 21%. The gap to the previous verbs path remains;
Gufo model performance has not yet been measured with it.
The [NHI control channel prototype](../bench/results/2026-10-06-verbs-control-ring.md)
built afterwards also passed the full matrix. Its isolated control PING is only
slightly faster than XDomain. The test NHIs have no free rings beside two data
rails, so reserving one control ring per connection makes large transfers worse.
The prototype stays disabled separately (`dmabuf_control_ring=0`).
The [actively sent DONE on XDomain](../bench/results/2026-10-07-xdomain-done.md)
variant needs no additional NHI rings and also stays disabled separately
(`dmabuf_push_done=0`). The [hardware check from October 8](../bench/results/2026-10-08-hardware.md)
passed with GPU and host MRs, remote errors, RNR and deliberately lost DONEs.
At 10,304 B, the measured cycle latency dropped by 16.6%; large transfers stayed
practically level. After a forced link teardown the data remained correct, but
normal interrupts and latency only returned after an ACPI NHI reset.
The measurements from October 2–4 in
[GPU_RDMA_STATUS_2026-10-04.md](GPU_RDMA_STATUS_2026-10-04.md) concern the
earlier exclusive ioctl path and must not be read as measurements of this
integration. The default mode remains `dmabuf_verbs=0`.

## TCP and previous attempts

Gufo has already removed the TCP header and TCP acknowledgment per normal verbs
exchange: the "Header-free RDMA exchange" experiment in
`docs/models/qwen3.8-flash-next/EXPERIMENTS.md` uses WRITE_WITH_IMM and three
windows. It documents 46 → 24 µs for one row as well as +2.5% AR and +4.3% MTP.
That is a different experiment from the direct HIP DMA-BUF adapter.

The previous direct adapter coordinates readiness and operations over TCP.
Its transport diagnostic was slower than normal verbs despite the removed
payload copies. The retained prefill overlap and the discarded variant with
fine-grained coherent RX are also documented; see the status file. It explicitly
named DMA-BUF import behind verbs as the next piece of work.

By default, the new driver mode uses the existing Thunderbolt XDomain
configuration channel for PREPARE, STATUS and ABORT. It needs no TCP per WR.
Applications may still use an initial TCP channel for GIDs, QPNs, IOVAs and
rkeys. The new test closes this channel before the first WR.

The configuration channel needs at least one preparation and one acknowledgment
per batch; STATUS queries are possible in addition. WRs are currently serialized
through a global ordered workqueue. This demonstrates neither lower latency nor
an improvement in Gufo prefill. "No TCP" and "no copy" do not replace measuring
these costs.

## Actively sent DONE on XDomain

`dmabuf_push_done=1` negotiates the `PUSH_DONE` capability through HELLO.
If both peers support it and no separate control ring has been negotiated,
PREPARE, its readiness response and ABORT stay on the existing XDomain
configuration channel. The receiver sends each fully placed batch there as DONE.
On the test NHIs this path can still use `lanes=2`, and thus four data rails in
total. It creates no further NHI rings or HopIDs and no periodic control worker.

The source registers the complete transaction before PREPARE. A completion
message that arrives early stays stored; a successful source CQ still also
requires the local TX DMA completion. An empty WR completes directly in the
PREPARE response, without an additional DONE frame or STATUS round trip. If the
message is missing after 10 ms, the existing STATUS query confirms the cached
placement. A failed send of a DONE message does not discard this placement
information and does not lead to retransmission of the payload.

DONE is consumed before the general XDomain response matcher. That matcher checks
the route and protocol UUID, but not our token or opcode. Late or malformed DONE
messages must therefore not answer another PREPARE or HELLO request either. Only
fully validated messages with a matching boot generation, domain, QPs, PSN, rails
and batch wake the registered transaction. Normal responses stay with the existing
matcher; `peer->control_lock` still serializes these requests. Software references
keep the RX completion tracker valid even when a connection is torn down on another
data link; the hardware rings are still stopped and freed immediately on peer
teardown.

The mode passed the hardware cases above and stays off by default.
A fast automatic restart after link teardown is not qualified.
For an isolated test, the lab script sets
`TBV_LAB_PUSH_DONE=1 TBV_LAB_CONTROL_RING=0 TBV_LAB_LANES=2`.
Debugfs shows `dmabuf_push_done`, per peer `push_done: ... ready=1`, and
`control_done_rx` and `control_notify_failed`. The existing phase measurement
separates `done_wait` from STATUS requests that were actually necessary.
The plain PING measurement stays on XDomain and should not get a shorter round
trip from this mode alone. The mode saves messages per batch; any benefit for
Gufo still has to be measured.

## Separate NHI control channel

`dmabuf_control_ring=1` negotiates the additional capability through HELLO.
If both peers support it, XDomain sets up a dedicated control tunnel through
HopID exchange and readiness confirmation. After that, PREPARE, the readiness
response, ABORT and actively sent DONE run over dedicated NHI rings. If the
capability is disabled on one peer, communication stays on the previous XDomain
path. A DMA that has already started does not switch transport.

The channel uses one TX and one RX ring with 128 entries per physical connection,
32 pooled TX buffers, 64 permanently posted RX buffers and 64 prepared event
slots. A message contains 256 bytes of host metadata. GPU payload stays on its
exclusive data rings. RX callbacks copy and repost their small host buffer; a
separate worker prepares the GPU receive. Up to 16 control transactions are matched
by full token, boot identities, domain, QPs, PSN and batch. The global serialization
of payload WRs and the single active RX WR remain in place.

The source registers DONE before PREPARE and keeps an early message until the local
TX DMA completion. The receiver actively reports each fully placed batch. Only then
may the source continue. The verbs CQ on the receiver still appears once for the
whole WR. Empty ownership-transfer WRs complete immediately under the same RX lock.
The existing QP, PSN, PD, rkey, range and lifetime checks apply here as well.

After 10 ms without DONE, the source queries the cached STATUS as recovery. The
normal exchange does not need this query. The prototype additionally checks ring
progress every millisecond through the existing NHI workers and, when the TX pool
is temporarily occupied, waits for it to be released. This fixes control TX
completions that failed to arrive in the hardware test, but causes additional CPU
work.

**Ring budget on fuzzy and misty:** each NHI reports three hops. Hop 0 belongs to
the configuration channel; two data rails occupy the remaining rings. For this
prototype, therefore use `lanes=1` on both hosts: one data ring and one control
ring per NHI, two data rails in total across both cables. Without a free ring the
requested channel is rejected. As long as it is not ready, GPU WRs are rejected
with `ENOTCONN`. Before testing, check `control_ring: ... ready=1` for each
connection in debugfs `peers`.

The root-protected measurement function needs no GPU allocation. It runs
1–100,000 control PINGs on the given peer and returns mean, minimum, median, P95,
P99 and maximum in nanoseconds. A module parameter makes the measurement possible
under the existing kernel lockdown:

```sh
echo '1 50000' | sudo tee /sys/module/thunderbolt_ibverbs/parameters/dmabuf_control_bench
sudo cat /sys/module/thunderbolt_ibverbs/parameters/dmabuf_control_bench
```

Debugfs `peers` shows ring readiness, errors, sent/received control frames,
PREPARE, DONE, STATUS, unanswered matches and pool pressure. `control_tx_ns` and
`control_rx_work_ns` are elapsed handling times, not isolated CPU cycles. The
separate measurement and the comparison at equal and at full data rail count are
in the results report.

## Prepared SEND credits

`dmabuf_credits=1` (default 0) removes the PREPARE round trip from the critical
path for GPU SENDs. It requires `dmabuf_push_done=1`, `dmabuf_control_ring=0` and
the `DMABUF_CREDIT` capability negotiated through HELLO on both peers; otherwise
the previous flow stays active. The frames of an NHI RX ring land in the order of
the posted descriptors, so a destination must be known before the data. For SEND,
the receiver knows it: it is the next receive WQE. For WRITE, only the sender
knows it; WRITE keeps PREPARE.

Flow:

1. After a successfully placed GPU SEND or SEND_WITH_IMM, the receiver remembers
   the QP, length and lanes. If the next WQE is already available or gets posted
   later (`ibv_post_recv()`), the driver checks it as with PREPARE (QP, PD, lkey,
   access rights, range, 4 KiB alignment) and arms its GPU pages for exactly this
   length and the expected PSN. The WQE stays posted. The receiver then sends
   CREDIT to the sender.
2. A SEND with the same destination QP, PSN, same length and same lanes uses the
   credit. The sender reports COMMIT one-way with the complete WR metadata and
   posts its frames immediately, without waiting for a response. The receiver only
   takes over the WQE on COMMIT; replay watermark, QP, PSN and lane checks apply as
   with PREPARE. DONE and STATUS recovery stay unchanged.
3. If a PREPARE crosses a credit still in flight, the receiver takes over the
   arming for this PREPARE and names the credit in READY. The sender then discards
   the credit.
4. A credit that the next WR cannot use (different length or operation, multiple
   batches) is released before that WR's PREPARE: the sender fills exactly the
   armed frames with zeros and reports RELEASE. A non-matching PREPARE receives
   BUSY and triggers a CANCEL. QP destruction or a QP error on the receiver also
   requests the release and waits up to 500 ms. Without RELEASE, the rails are
   stopped as with any abort after arming.
5. If a PREPARE receives RNR, the sender waits up to 2 ms for a credit instead of
   sleeping 1–2 ms and asking again. Posting the WQE on the receiver thus becomes
   the signal to send.
6. If no other TX job is queued or active, `ibv_post_send()` itself takes a
   matching credit, sends COMMIT and posts the frames (`dmabuf_direct_post=1`,
   default). The TX worker then only waits for DMA and DONE and completes the WR.
   This saves waking the worker before the data. One lock covers the idle check,
   direct posting and queueing, so later WRs stay behind it.

Credits apply to one batch only and to at most `dmabuf_credit_max` bytes
(default 1 MiB). There is at most one armed credit per receiver, just as there is
only one active RX transaction. The credit number sits in the previously reserved
field at byte 164; older peers do not negotiate the capability.

**Memory contract in credit mode:** while a GPU receive WQE is posted, a release
can overwrite its first `ALIGN(L, 4096)` bytes with zeros, where L is the length of
the previous SEND on this QP. This only happens when the following WR does not use
the credit. Bytes beyond the length of a SEND that is later received shorter are
then no longer the values written before posting. Anyone who expects contents
behind the received length must not use this mode.

Debugfs `summary` shows `dmabuf_credits`, `dmabuf_credit_max` as well as
`dmabuf_credit_granted`, `committed`, `adopted`, `released`, `canceled`,
`aborted` (receiver) and `used`, `returned`, `stale`, `direct` (sender). `mismatch`
counts unexpected credit messages; their lanes are stopped.
Profiling tracepoints: `credit_grant`, `credit_received`, `credit_commit`,
`credit_commit_rx`, `credit_release`, `credit_release_rx`, `rnr_credit_wait`,
`tx_direct`.

## Interface and memory contract

- Native RC QPs: SEND, SEND_WITH_IMM, RDMA_WRITE and RDMA_WRITE_WITH_IMM.
  RDMA_READ, UC, GSI, SRQ and GPU scatter/gather are not yet supported in this
  mode. The default mode keeps its existing operations.
- MR import: valid DMA-BUF FD, offset, length and IOVA with page alignment;
  at most 256 MiB per exported object. Permitted access rights are LOCAL_WRITE and
  REMOTE_WRITE; REMOTE_WRITE requires LOCAL_WRITE. Key and PD are checked before
  placement. An import is an MR, not an exclusive device.
- GPU WR: one contiguous SGE whose offset in the MR is a multiple of 4096. Direct
  GPU lengths must be multiples of 32 bytes, including 0; at most 248 MiB. One-byte
  transfers corrupted guard bytes behind the payload in the hardware test, so such
  lengths are rejected before DMA. Short last frames with 32-byte alignment are
  allowed. RX needs a fully backed 4 KiB page even for a short remainder. A GPU
  SEND receive WQE must reserve this rounded-up span; its CQ completion reports
  the logical length.
- The RDMA core registration pins the exporter. Additional static attachments are
  bound to the actual NHI devices. Only their DMA addresses are used in
  descriptors. No `vmap()` mapping and no CPU synchronization of the GPU payload.
- MR and QP references persist until the end of the DMA operation concerned. Errors
  and aborts drain descriptors before release. As usual, applications must keep MRs
  and buffers available until completion, and may only release GPU consumer windows
  for DMA again after their reads.
- Normal host MRs work in the same mode through bounded coherent staging. This part
  copies payload and is not a direct GPU path.

GPU visibility remains the application's job. Before TX, a system release by the
producer is required; after RX, the GPU read must be correctly ordered. The test
adopts the proven timing-enabled `hipEventReleaseToSystem` and uncached RX. That is
the existing acquire probe for this hardware and HIP runtime, not a portable
guarantee from MR registration or the CQ alone.

## Placement, ordering and errors

A WR is split into batches of at most 512 frames per rail. At most four rails fit
into a 252-byte configuration record plus 4 bytes of CRC; with four rails, a batch
contains at most 8 MiB. Domains are matched through physical UUIDs and HopIDs, not
through local enumeration numbers.

PREPARE checks QP, PSN, PD, rkey, access rights and range, consumes the matching
receive WQE exactly once and arms RX. Only then does TX start. One token per WR and
immutable metadata make repeated control requests idempotent. Boot identities and a
replay watermark prevent old PREPAREs after a new WR. A changed remote boot state on
rings that still exist is rejected; both connections must be recreated.

A successful SEND completion requires local descriptor completions and confirmed
placement of all batches at the peer. SQ ordering and signaled/unsignaled handling
use the existing verbs queue. IMM and receive CQ appear once for the whole WR. RNR is
handled through PREPARE and the existing QP retry settings. Plain WRITE consumes no
receive WQE; the user must coordinate the destination buffer lifetime themselves.

**The existing retransmission of framed verbs payload does not yet carry over to raw
GPU frames.** Missing, truncated or corrupt frames complete as errors; payload is not
blindly sent again. A final marker per rail detects shifted frames after a loss.
On a data error or an abort after arming, both directions of the affected rails are
stopped. Both test modules or physical connections must then be reloaded. Clean,
fully completed WRs and clean QP changes, by contrast, do not use up the rails.

Further open qualification: physical hot unplug, GPU reset, IOMMU errors, parallel
QPs and sustained serving load. The mode is currently intended for trusted native
test peers.

### Loss and reconnection (as of October 18)

The module parameter tool `dmabuf_fault` (test only, empty by default; no debugfs,
because lockdown blocks writable debugfs files) drops, delays, duplicates or fails
the next messages of a type that this host sends:
`echo "commit delay ms=50 count=1 skip=20" > /sys/module/thunderbolt_ibverbs/parameters/dmabuf_fault`.
Types: `credit cancel commit release done`; actions: `drop fail delay dup`;
`clear` deletes all rules, reading shows rules and hits. `peers` shows
`gpu_stopped=1` per rail for stopped rails, and the kernel reports the stop.

Survived without a reload: lost, delayed or duplicated DONE (STATUS); lost, delayed
or duplicated CREDIT (PREPARE takes over, CANCEL carries all the data); lost CANCEL
(BUSY triggers the release); RELEASE up to 300 ms; COMMIT with at least up to 2 s of
delay (STATUS answers BUSY while a matching credit waits for its COMMIT); QP
destruction, process kill and reload of the peer module with an armed credit. The
rails on both sides are stopped on: lost COMMIT; lost RELEASE or RELEASE after more
than 500 ms; CREDIT and CANCEL lost together; failed send of a CREDIT, CANCEL, COMMIT
or RELEASE message; duplicated COMMIT or RELEASE; process kill during a transfer; and
teardown of a link. Both modules must then be reloaded; rebinding the services is not
enough. Open WRs on the other side can stay without an error CQE for 30 s and longer.
Details: `bench/results/2026-10-18-verbs-loss.md`.

## Build and test

`tools/build-gpu-verbs.sh` builds `userspace/bench/tbv_gpu_verbs.hip` against an
existing HIP and verbs installation. `HIPCC`, `HIP_PATH`, `HIP_DEVICE_LIB_PATH`,
`VERBS_INCLUDE` and `VERBS_LIB` are provided for separate SDKs. The protocol probe
`tools/ci/verbs-dmabuf-smoke.c` is part of the existing Nix/distribution checks.

In a coordinated maintenance window, on **both** hosts save the loaded module state,
build ID and parameters, stop device users, and first unload the module on **both**
hosts. Only then load the signed test modules. Unloading and loading on each host at
the same time is not enough: a new host can still learn the old peer identity and
then reject the new generation.

```sh
sudo insmod kernel/thunderbolt_ibverbs.ko \
  profile=linux_perf tbnet=prefer_rdma lanes=2 \
  bind_services=1 allocate_rings=1 start_rings=1 \
  negotiate_native=1 enable_tunnels=1 register_verbs=1 \
  dmabuf_verbs=1

# Rank 0, then rank 1; insert the management IP:
tbv_gpu_verbs --device usb4_rdma0 --gid-index 1 --iterations 8 --rounds 2 --remote-error
tbv_gpu_verbs --device usb4_rdma0 --host MANAGEMENT_IP \
  --gid-index 1 --iterations 8 --rounds 2 --remote-error
```

The test checks all four operations, eight invalid imports, a valid subrange, FD
offset 4096, IOVAs independent of GPU VAs, foreign PDs, unaligned SGEs and GPU
lengths, three outstanding windows, unsignaled WRs with a later signaled completion,
QP/MR recreation, byte lengths and IMM ordering. After GPU pre-reads before RX, every
payload byte and a 64-byte guard are verified. Only a 16-byte check result is
transferred to the host. `--remote-error` checks a wrong rkey with an error CQ, also
for an unsignaled WR; `--rnr-delay-ms 100` delays the first receive readiness on
rank 0.

`--host-mr` checks the same flow with host MRs and HIP copies and serves as a control
for the old mode or the new staging path. Reported times include GPU generation,
pre-read, transfer and full verification at depth 3; they are not pure DMA time.
`--bytes N --op WRITE_IMM` selects a single size and operation; both peers must use
the same selection.

`--depth 1|2|3` selects the number of outstanding windows. With `--op WRITE_IMM`,
`--persistent-rq` keeps four generic zero-SGE receive WQEs posted and replenishes them
when reaping the CQ, like Gufo's verbs adapter. With `--op SEND` or `--op SEND_IMM`,
the windows themselves are the receive WQEs: each window is reposted after full
verification and pre-warming of the next pass. This posting returns the window; a
per-message barrier is not needed. Use the option without error or RNR delay probes.
The message is still checked for IMM, length, order, payload and guard bytes. A next
barrier that arrives early is noted until the next iteration. The barriers still keep
DMA placement behind the release of the GPU consumer window.

`--spin-us 5000` first polls for a CQ completion actively and only then sleeps 50 µs
between empty polls. Gufo also uses an initial 5 ms spin window, there for the whole
exchange; the test sets it per CQ wait call. The default is `--spin-us 0`.
`--late-receives` reproduces the earlier late provision of receives and cannot be
combined with `--persistent-rq`.

Additional `PHASE` lines split the reported total duration into eight consecutive CPU
time sections: early RQ posting, submitting GPU jobs, TX system release,
readiness/barrier, submitting data WRs, reaping CQs, RX copy/release and full GPU
verification. The values are normalized per message. `tx_release_us` also includes
producer kernels still running; `gpu_submit_us` may include synchronously executed
HIP copy work in host MR mode. These are not GPU event runtimes of individual kernels.

## Gufo and further optimization

Detailed WR/batch/rail tracepoints, disabled by default, and optional GPU event
measurements in the test are described in
[TRANSPORT_PROFILING.md](TRANSPORT_PROFILING.md). The capture runner records completed
diagnostic cases in a dedicated tracefs instance; the report provides distributions, a
timeline and loss/fallback checks. `TBV_PROFILING=0` or the Nix package
`thunderbolt-ibverbs-no-profiling` removes the new detail instrumentation from the
module.

Gufo's Qwen3.8-Flash-Next TP2 all-reduce transfers `rows * 2560 * sizeof(float)`.
The existing verbs adapter additionally writes a 64-byte header at the start of a
32 MiB window. Both the total WR length and arbitrary row counts therefore satisfy
the 32-byte rule; the three windows start on page boundaries. The earlier direct ioctl
adapter even rounds the transfer up to 4096 bytes. The lengths therefore do not limit
this Gufo path. Its verbs adapter still uses host MRs; HIP allocation, DMA-BUF
registration and GPU-appropriate header accesses must also be integrated there.

`dmabuf_profile=1` records call count, total nanoseconds and maximum per phase in the
debugfs `summary`: queue, job, TX/RX descriptor build, PREPARE, TX posting, TX wait,
STATUS, release, verbs completion, as well as separate PREPARE, RNR and STATUS wait
pauses and `done_wait` with actively sent DONE. The values are cumulative since the
module was loaded; queue and job, and RX reaping and RX release, are overlapping
measurements and must not be added into a total. Profiling is off by default.

`dmabuf_index=1` indexes the DMA addresses once per MR and actual NHI device. The
entries preserve SG boundaries and the permitted length of each frame.
`dmabuf_index=0` uses the previous list search for A/B measurements. The MR references
keep both attachments and index until the descriptor drain.

`dmabuf_rx_eager=1` schedules a separate worker on the last RX DMA callback, which
cleans up the batch and releases the receive CQ. It runs outside the ring callback and
under the same RX lock as STATUS. On the previous control path, the source confirms
placement through STATUS. The control ring sends DONE; the XDomain push mode also
actively reports placement. Both complete empty WRs already at PREPARE; these have no
DMA callback. `dmabuf_rx_eager=0` allows comparison with the previous STATUS reaping.

`dmabuf_parallel_post=1` hands batches of 128 frames or more to different actual NHI
devices in parallel. Rails of the same NHI stay ordered within a group; the worker
switches to the next rail after at most 16 descriptors each, so that their DMA overlaps
earlier. A dedicated unbound workqueue enables the parallel work; the caller waits for
all groups, including on errors, before batch metadata is released. Small batches still
use sequential posting. `dmabuf_parallel_post=0` turns this comparison mode off. Index,
eager RX and parallel posting are on by default in the experimental mode; profiling is
off.

The NHI control channel did not confirm the suspected large saving over XDomain in this
serial way of working. In the hardware comparison, XDomain DONE saved normal STATUS
queries while keeping all data rails; its latency advantage was limited to small
transfers. The disturbed interrupt state after link teardown needs its own
investigation. Further possibilities are fewer worker switches and batched queueing of
descriptors. Prepared RX windows with credits and overlapping batches also remain
possibilities. For large WRs, the phase measurement shows costs for TX/RX posting and
DMA in particular; for small WRs, control and synchronization dominate.

On the APUs, less copy work can free memory bandwidth for the model kernels. Any
benefit for Gufo must be checked under model load with prefill time and tokens per
second; the transport diagnostic alone does not measure it.

The lab scripts `tools/lab-verbs-dmabuf-module.sh` and `tools/lab-gpu-verbs-run.sh`
prepare reversible module swaps and the existing Nix runtime in the isolated test
directory. They do not replace a coordinated maintenance window and install neither
modules nor libraries system-wide.
