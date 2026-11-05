# Pre-armed SEND credits for direct GPU verbs, 16 October 2026

This follows up [the parity checkpoint](2026-10-16-verbs-parity.md), item 4
of its next steps: move receive preparation off the critical path with a
specified, credit-controlled pre-armed receive window. The new opt-in mode
`dmabuf_credits=1` does that for GPU SEND and SEND_WITH_IMM. It passed the
ordinary-verbs checks on fuzzy and misty. In this session's matched runs,
direct GPU SEND_IMM with credits measured 87.8 µs at 32 B and 91.0 µs at
10,304 B per checked message. This session's host-memory control measured
93–141 µs for its best small-message pattern. **WRITE is unchanged**, and Gufo
still uses WRITE_WITH_IMM, so this does not by itself deliver parity for Gufo.
Defaults are unchanged.

## Why SEND, and what is armed

An NHI RX ring places frames into its posted descriptors in order. Nothing in a
frame redirects it, and E2E flow control stays off on the AMD test hosts. The
receiver must therefore know the target pages before data arrives. For WRITE
only the sender knows them. That is why every direct WRITE needed
PREPARE/READY, about 22 µs at P50 in the earlier small-message trace. For SEND
the target is the receiver's next posted receive WQE.

After a GPU SEND completes, the receiver remembers the QP, length and lanes.
When the next WQE is posted, or immediately if one is already posted, it runs
the same validation as PREPARE: QP state, PD, lkey, access, bounds and 4 KiB
alignment. It then posts RX descriptors for the expected PSN and exact length,
and sends CREDIT. The WQE stays posted until the WR arrives. A SEND with the
same QP, PSN, length and lanes sends a one-way COMMIT and posts its frames
immediately. The receiver claims the WQE on COMMIT, under the same replay
watermark and QP/PSN checks as PREPARE. DONE and the STATUS fallback are
unchanged. The [design and memory contract](../../docs/GPU_VERBS.md#prepared-send-credits)
also cover the other cases:

- A PREPARE that raced a credit still in flight adopts the armed pages, and
  READY names the credit so the sender drops it.
- A credit the next WR cannot use is filled by the sender with exactly the
  armed frames, followed by RELEASE. The receiver asks for this with CANCEL
  when it gets a non-matching PREPARE, or when the QP is destroyed or in error.
- An unresolved release after 500 ms, a lost CREDIT, bad frames or a COMMIT
  for an unknown credit quarantine the rails, as any abort after arming
  already does.
- On RNR, the sender waits up to 2 ms for a credit instead of sleeping and
  retrying. Posting the WQE becomes the send signal.

At most one credit is armed per receiver, matching the single active RX
transaction. Credits cover one batch and at most `dmabuf_credit_max` bytes
(default 1 MiB). The wire format adds three notification opcodes and a credit
sequence in the previously reserved field at byte 164. Both peers must
advertise the new HELLO capability; otherwise nothing changes.

**Contract difference:** a release writes zeros over the first
`ALIGN(L, 4096)` bytes of a still-posted GPU receive WQE, where L is the
previous SEND length on that QP. That only happens when the following WR
cannot use the credit. Bytes beyond the length of a later, shorter SEND then
no longer hold their pre-post values. The verifier's size matrix never
shrinks a credited length, so its guard checks do not exercise this case.

## Correctness

Both hosts ran the credit module (`dmabuf_verbs=1 dmabuf_push_done=1
dmabuf_credits=1 lanes=2`, 7.2.10, two cables/four rails) and passed:

- The full verifier matrix: SEND, SEND_IMM, WRITE_IMM and WRITE, 11 sizes,
  depth 3, two rounds with QP/MR recreation, complete payload and 64-byte
  guard checks, plus the unsignaled invalid-rkey `REM_ACCESS_ERR`/flush probe.
  Size changes exercised the release path: 26–27 releases with filler and
  10–12 cancels per host, no aborts and no mismatches.
- Delayed receive (`--rnr-delay-ms 100 --signal-all`) and host-MR staging
  with its invalid-key error case.
- All timing cases below, with every message verified.

Every passing run reports `tcp_data_calls=0`. The final local smoke suite
passes. The RX-state harness now compiles the production credit functions and
covers arming, the post_recv path, COMMIT, adoption, cancel/release, an
unknown COMMIT, teardown, a lost CREDIT and disconnect. Both host builds pass
with `W=1`. After restoring the original module, taint stayed 4096, and no
BUG/Oops/WARNING, GPU-reset or IOMMU-fault messages were logged.

Two defects were found and fixed on hardware before the measurements:

- COMMIT was sent without the reply bit. The tb core traces showed it, and the
  receiver consumed it as a notification and dropped it silently. Unknown
  notification opcodes now count as `mismatch`.
- Arming queued from `ibv_post_recv()` ran on that CPU's high-priority queue.
  The poster then spun on its CQ and delayed the worker by 1–2 ms. It now uses
  the unbound high-priority posting queue.

## Measurements

The verifier gained a persistent SEND mode (`--persistent-rq` with SEND or
SEND_IMM). Each window's receive WQE is reposted once its consumer has
verified it and the next prewarm has finished. That repost returns the window,
so no barrier SEND is needed per message. WRITE_IMM keeps the parity
checkpoint's flow: a barrier SEND plus WRITE_WITH_IMM into zero-SGE persistent
WQEs.

Same verifier binary, flags (`--spin-us 5000`, four rounds with QP/MR
recreation) and size plan as the parity checkpoint. Small sizes use depth 1
and 128 iterations; 1 MiB uses depth 3/32, 20 MiB + 64 B depth 3/16 and
32 MiB − 64 B depth 3/8. Values are medians of the six rank/round averages
from rounds 1–3, in µs per message, including GPU production, prewarm,
transfer, consumption and full verification. Governor and frequencies were
left as found. All cohorts ran in one session.

| Bytes | Direct SEND_IMM, credits | Direct SEND_IMM, no credits | Host SEND_IMM | Direct WRITE_IMM, credits module | Direct WRITE_IMM, no credits | Host WRITE_IMM |
|---|---:|---:|---:|---:|---:|---:|
| 32 | **87.81** | 1,148.74 | 93.30 | 130.00 | 134.66 | 102.22 |
| 10,304 | **91.04** | 1,279.47 | 26,915.90¹ | 145.70 | 154.82 | 140.91 |
| 1,048,576 | **327.83** | 399.12 | 556.11 | 366.30 | 412.66 | 512.29 |
| 20,971,584 | 6,403.23 | 6,527.04 | 9,811.75 | 6,505.61 | 6,567.44 | 6,443.68 |
| 33,554,368 | 10,060.02 | 10,286.91 | 16,348.01 | 10,308.44 | 9,996.17 | 10,576.43 |

¹ Range 111.7–45,710 µs. The unmodified baseline module showed the same
millisecond stalls (9,075 µs median). It also measured 119.57/134.78 µs for
host WRITE_IMM at 32/10,304 B. Tonight's host-path numbers are therefore
higher than the 100–110 µs of the earlier report because of run-to-run and
day-to-day variation, not because of this change. The host and direct paths
here are both slower when a SEND meets an empty RQ. The direct path without
credits pays a 1–2 ms RNR sleep per early SEND.

Readings:

- In the barrier-free SEND pattern, credits cut small messages from
  1.15–1.28 ms to about 88–91 µs. Most of that is RNR polling replaced by the
  credit wakeup. In the phase split at 10,304 B, the WRITE_IMM flow spends
  26.7 µs waiting for its barrier (`ready`) and 47.8 µs in `cq_wait`. The
  credited SEND flow has no barrier and spends 39.5 µs in `cq_wait`.
- About 60% of small-message credits were committed and 40% adopted by a
  racing PREPARE. At 1 MiB/depth 3 most were adopted, which still skips the
  RX arming on the critical path.
- WRITE_IMM does not use credits; its direct-path numbers with and without
  the module parameter match within run-to-run variation.
- Credited direct SEND_IMM at 32/10,304 B was below every host-path
  small-message result in this session. These are bounded diagnostic
  comparisons on two hosts without locked clocks, not a universal latency
  claim. The flows differ: SEND hands ownership back with the receive WQE,
  WRITE_IMM with an explicit barrier.

## Follow-up, 17 October: Gufo's exchange and direct posting

Gufo branch `feat/verbs-dmabuf-sendimm` (based on `origin/rdma`, not pushed)
moves its three exchange windows to uncached DMA-BUF device memory when both
ranks can register it, and sends each partial with SEND_WITH_IMM into the
peer's posted window. Otherwise, as on today's production module, it keeps
host windows and WRITE_WITH_IMM. Two Gufo changes affect both window types:

- Exchanges finish without waiting for their own send completion.
- Spin waits yield every 64 iterations. A spinning exchange thread had held
  off the per-CPU worker that finishes USB4 ring completions for a scheduler
  slice: about 3 ms p99 tails, which disappeared when the processes were
  pinned away from the Thunderbolt interrupt CPUs.

Driver traces then showed the remaining single-row gap came from two worker
wakeups: the TX worker before a credited SEND (2.3–2.9 µs) and the RX
completion worker (1.8–2.3 µs). The driver now posts a credited single-batch
SEND from `ibv_post_send()` when no other TX job is queued or running
(`dmabuf_direct_post=1`, default). The TX worker then only waits for DMA and
DONE. A submit lock covers the idle check, direct posting and queueing.

`tp_batched_probe --allreduce-bench 1000` runs Gufo's production queued
exchange (stage kernel, host transfer thread, wait kernel, add) and checks
every sum exactly. Four alternating rounds per mode, p50 in µs, median of
eight rank/round values; all 96 rank-runs verified:

| Rows (bytes) | Device windows, direct post | Device windows, TX worker | Host windows (production path) |
|---|---:|---:|---:|
| 1 (10,240) | **29.5** | 32.4 | 32.0 |
| 2 (20,480) | **31.8** | 34.0 | 41.8 |
| 8 (81,920) | **45.6** | 45.8 | 54.6 |
| 512 (5,242,880) | **1,316** | 1,314 | 1,419 |

Single-row p99 was 52–83 µs with direct posting, 51–66 µs on host windows and
70–101 µs via the TX worker. Direct posting covered 2,990–2,995 of 3,000 WRs
per host and run. These are exchange-level numbers; deferred completion
means minimum times can exclude a rank's own transfer, so they are not
reported. Model throughput and token-level agreement are still unmeasured.

The full verifier matrix, remote-error, RNR-delay and persistent SEND_IMM
cases passed again with direct posting. Evidence is in
[gufo/](2026-10-16-verbs-credits/gufo/): logs and debugfs snapshots for
every run, including those with the rejected early TX-ring kick. Large
traces are not kept.

## Status and next steps

- The receive-window protocol is specified and implemented for SEND within
  the existing verbs QP/MR/CQ contract. No new user API was added; an
  application uses it by sending into posted receive WQEs.
- **Gufo:** the adapter change is on a Gufo branch (see the follow-up above).
  Its exchange is at or below the host path at every measured size. The next
  check is end to end: tokens/s, prefill and token-level agreement.
- WRITE remains PREPARE-bound. Removing that round trip would need the sender
  to publish targets ahead of time, which is a different protocol.
- Loss/reconnect qualification, parallel QPs with credits, and GPU reset
  during an armed credit are not covered. The exclusive ioctl prototype stays
  until its use cases run through verbs.

## Evidence

[Raw logs, debugfs before/after, traces, scripts and hashes](2026-10-16-verbs-credits/).
`timings.txt` is the output of `summarize.py`. Rejected states are kept:
`smoke`–`smoke4` (missing reply bit), `credit2-*` (per-CPU arming delay) and
`nocredit-sendimm`. The original modules and parameters were restored on both
hosts, with matching build IDs.
