# Direct GPU memory through verbs: driver results, 16 October 2026

The existing DMA-BUF RC implementation passed the driver-level ordinary-verbs
checks on fuzzy and misty. Interleaving descriptor posting across sibling rails
reduces large-transfer time and brings the tested 1–32 MiB cycles near the
normal host-memory path. **Small-message parity is not achieved.** This does
not qualify Gufo integration or every operation supported by ordinary RC.

## Retained change

The old posting worker filled one rail before starting the next rail on the
same NHI. The new worker posts at most 16 descriptors per turn, alternating
between that NHI's rails. Independent NHIs still use separate workers.
Per-ring frame order, descriptor callbacks, MR/QP references, remote placement
confirmation and error draining are unchanged. All posting workers finish
before batch storage can be released. Small batches keep the existing serial
posting path.

The change adds no transport mode or wire-protocol version. A production-body
smoke test checks ordering, sibling progress, empty/unequal/tail lengths,
controller isolation, failures and TX counters with ASan/UBSan.

Profiling now identifies a rail by its unique wire-lane index within a batch,
rather than its per-link rail number, which collided across the two cables.
`lane_post_chunk` measures each posting turn; `lane_post` measures the full
elapsed posting interval of one rail, including intervening sibling turns.
These intervals overlap and must not be summed as CPU time.

Rejected experiments: using one rail below 64 KiB and raising the ordered TX
worker's priority. Neither established a stable small-message improvement;
neither remains in the source.

## Matched measurements

Both hosts: Strix Halo, Linux `7.2.8-100.fc43.x86_64`, two cables/four data
rails. Candidate and control use the same HIP/verifier binary, provider and
settings. Verifier SHA-256:
`870bed5e2742d0eb2c7b8501c1cfcacfeccabb4f3aa38df9b9019d641f966baf`.
The verifier source is archived with the results. It predates the optional
userspace detail-profiling additions; none of those were needed for these runs.

Direct mode uses `dmabuf_verbs=1 dmabuf_push_done=1 dmabuf_control_ring=0`;
indexing, eager RX and parallel posting are on. The host control uses
`dmabuf_verbs=0`. Both use `lanes=2`, `--op WRITE_IMM --persistent-rq
--spin-us 5000`. Kernel summary profiling and tracepoints are off in the
following cohorts. Small sizes use depth 1 and 128 iterations per round;
1 MiB uses depth 3/32 iterations, 20 MiB+64 B depth 3/16, and 32 MiB−64 B
depth 3/8. Each case has four rounds with MR/QP recreation.

Values are medians of the six rank/round averages from rounds 1–3, in µs per
message. Round 0 remains in the raw logs as warmup. Ranks are correlated.
Two separate candidate/control runs are shown, rather than hiding run-to-run
variation in one average. The sequence included diagnostic/rejected experiments
between the A and B comparisons; every result remains archived.

| Bytes | Direct A | Host A | Direct B | Host B |
|---|---:|---:|---:|---:|
| 32 | 132.38 | 99.98 | 164.32 | 103.16 |
| 10,304 | 141.67 | 109.91 | 181.52 | 107.53 |
| 1,048,576 | 408.17 | 435.67 | 398.74 | 425.39 |
| 20,971,584 | 6,517.91 | 6,428.12 | 6,507.34 | 6,404.37 |
| 33,554,368 | 9,963.81 | 10,192.85 | 10,426.27 | 9,995.92 |

The direct path is about 6% faster at 1 MiB, 1.4–1.6% slower at 20 MiB,
and between 2.2% faster and 4.3% slower at 32 MiB in these runs. Small messages
remain slower and show substantial variation. The powersave CPU governor was
left as found; frequencies were not locked. These are bounded diagnostic
comparisons, not a claim of universal latency parity.

A later unprofiled rerun of the exact pre-optimization module measured
454.19/7,419.75/11,132.02 µs at 1/20/32 MiB. Against candidate B, those are
reductions of about 12.2%/12.3%/6.3%. The initial profiled before/after 20 MiB
experiment independently showed 7,364.61 → 6,328.19 µs. Do not mix its absolute
timings with the unprofiled comparison.

The verifier includes GPU production, RX pre-reading, a verbs ownership
barrier, transfer, GPU consumption, complete payload verification and guard
checks. This is **not pure network latency**. The host control includes
GPU↔host staging copies; Gufo's GPU-mapped host buffers are a different
application comparison. Kernel worker CPU and DRAM bandwidth were not
isolated by these measurements.

## Correctness and limits

Both hosts passed:

- Direct GPU MRs through `ibv_reg_dmabuf_mr()`, ordinary `ibv_post_send()` /
  `ibv_post_recv()`, and CQ polling: SEND, SEND_WITH_IMM, WRITE and WRITE_WITH_IMM.
- Complete payload/64-byte guard checks, import and SGE validation, explicit
  IOVAs/offsets, three outstanding slots and QP/MR recreation.
- Unsignaled invalid-rkey send completion as `REM_ACCESS_ERR` and corresponding
  receive flush; delayed receive/RNR testing with all sends signaled.
- Ordinary host MRs in the DMA-BUF mode's staging implementation, including
  its invalid-key error case.
- The normal host-memory verbs matrix with the modified driver and direct mode
  disabled. Its stricter invalid-key error contract was not requalified here.

Recorded matrix payload totals are approximately 1.97 GB per host per two-round
matrix, in addition to timing runs. All passing verifier runs report
`tcp_data_calls=0`. TCP is used only for initial connection metadata.
The full local protocol/state smoke suite and syntax checks pass. Both host
kernel builds pass with `W=1`. No new BUG/Oops/WARNING, GPU-reset or IOMMU-fault
markers were found in the retained kernel logs; taint stayed 4608.

GPU lengths remain multiples of 32 bytes and GPU SGE offsets page-aligned.
RDMA READ, UC and GPU scatter/gather are not implemented in direct mode.
Raw data-frame loss and fast reconnect are not newly qualified. Direct mode
remains opt-in; no defaults were changed. Existing limitations in GPU_VERBS.md
still apply.

## Remaining latency and architecture work

The native host path keeps packet receive buffers posted. It receives first
and copies to the addressed MR afterward. Direct placement needs the target
GPU pages armed before data arrives, so the current implementation does
PREPARE/READY and confirmed DONE per batch. Small-transfer traces show these
control exchanges dominate much of the additional transport time; removing
payload copies cannot offset them at every size.

Some ownership and completion coordination is necessary. A roundtrip for every
transfer is not a fundamental requirement of GPU memory: pre-advertised,
credit-controlled receive windows could move preparation off the critical
path. That is future protocol work, not a safe deletion of the current checks.
It must specify arbitrary target changes, slot reuse, bounds/keys, QP ordering,
MR deregistration, generations and failure recovery before enabling it.

Keep one verbs-facing QP/MR/CQ contract and share the correctness/lifetime
machinery where possible. Separate payload placement methods are reasonable;
separate full verbs stacks are not the intended final architecture. Retire the
exclusive ioctl prototype once the verbs GPU implementation replaces its
qualified use cases. Do not silently add a copied small-GPU fallback and call
it direct placement.

## Evidence and restoration

[Raw logs, source hashes, task-only patch and timing script](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-16-verbs-parity/README.md).
The exact original loaded module build IDs and all parameters were restored
on both hosts. Installed modules/libraries were not replaced; NHI resets were
not performed. The two application containers used automatic removal and
exited on shutdown. Their restart still requires the original launch command;
no guessed application configuration was started.

## Current integration status and next steps

This checkpoint includes the previously uncommitted GPU transport, DMA-BUF
verbs implementation, supporting provider/build/refactoring work, tests and
measurement records, together with the descriptor-posting optimization above.
It is an experimental merge candidate; it does not make direct mode the default.

Gufo should use **the same verbs adapter as InfiniBand**: the QP, SEND/WRITE
and CQ interface remains shared. Driver qualification is complete for the
operations and configurations listed above, but Gufo's ordinary verbs adapter
still uses GPU-mapped host buffers and has not been converted to DMA-BUF GPU
MRs. No Gufo source changes were made in this driver-benchmark iteration.
`ibv_reg_dmabuf_mr()` is a standard verbs registration API; this work does not
make an arbitrary GPU pointer usable through `ibv_reg_mr()` automatically.
InfiniBand GPU-memory support must be checked for the selected device/provider.

Next steps, in order:

1. Restore the Gufo workloads using their original launch commands. Drivers
   and parameters are restored; application containers are still stopped.
   The containers auto-removed on exit, and their complete launch configuration
   was not captured before stopping them.
2. Integrate DMA-BUF allocation/import and explicit GPU visibility/ownership
   into Gufo's shared verbs adapter. Adapt CPU header access for GPU buffers;
   retain host-memory registration when GPU DMA-BUF registration is unavailable.
   Keep transport selection and ordinary verbs operations shared with InfiniBand.
3. Qualify model correctness and end-to-end performance against Gufo's existing
   GPU-mapped host-buffer adapter. Driver verifier timings alone cannot establish
   application parity. Exercise supported InfiniBand configurations separately.
4. Reduce small-message control overhead with a specified, credit-controlled
   pre-armed receive-window protocol. Validate key/bounds checks, generations,
   ordering, slot reuse, MR/QP teardown and errors before claiming parity.
5. Finish loss/reconnect qualification and remaining operation coverage before
   broadening support claims or changing defaults. Retire the exclusive ioctl
   prototype only after its application use cases are covered through verbs.

Work stops at this documented checkpoint; these next steps are not implemented
or implied by the recorded benchmark results.

## Update, later on 16 October

- **Step 1 is done.** `systemctl --user start gufo@q8` on fuzzy starts both
  ranks from the saved configuration (`~/.config/gufo/head.env`); rank 1 runs
  as `gufo-rank1@q8` on misty. Both reported `rdma_ready` on usb4_rdma0, and
  the server listened again after the follow-up tests.
- **Step 4 is done for SEND, not for WRITE.** [Pre-armed SEND credits](2026-10-16-verbs-credits.md)
  (`dmabuf_credits=1`, opt-in) specify and implement the receive-window
  protocol for GPU SEND/SEND_WITH_IMM into posted receive WQEs. That covers
  arbitrary target changes, slot reuse, bounds/keys, QP ordering, teardown,
  generations and failure recovery. Direct SEND_IMM measured 87.8/91.0 µs at
  32/10,304 B in that session, below its host-path controls; WRITE still needs
  PREPARE. The measurements above are unchanged.
- Steps 2, 3 and 5 remain open. To benefit from credits, Gufo's shared verbs
  adapter would also post its GPU windows as receive WQEs and use
  SEND_WITH_IMM for decode-size exchanges.
