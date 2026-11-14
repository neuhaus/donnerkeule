# Gufo direct DMA-BUF integration, October 3–4, 2026

The experimental Gufo adapter runs GPU projection/epilogue → HIP TX DMA-BUF →
four NHI rails over two USB4 cables → HIP RX DMA-BUF → GPU sum on fuzzy/misty.
The complete Q8 model smoke matches the existing verbs path bit for bit. The
standalone verified exchange is slower and uses more process CPU time. Direct
two-lane prefill overlap recovers much of the first integration's model penalty.
Verbs remains Gufo's default; no production performance promotion is implied.

## Mechanism

Gufo's final attention/GDN projection and MoE epilogue receive the TX window
before launch and write into it directly. Three 32 MiB windows in each dedicated
96 MiB allocation provide the lifetime contract. TX is cacheable `hipMalloc`,
RX is `hipDeviceMallocUncached`. A timing-enabled system-release event precedes
the GPU ready flag. The CPU worker arms RX, exchanges scope/ordinal/shape and
readiness metadata over TCP, submits TX and waits for actual TX/RX completion.
The GPU consumes peer data after the arrival flag. No CPU payload mapping/copy
or GPU payload staging copy occurs. The final GPU sum still writes ordinary
model scratch, as required by the residual computation.

Prefill now uses Gufo's existing two-lane schedule above the 384-token threshold.
Each lane retains its direct lease while the other computes. Only the wait and
sum are deferred; release, shape checks and numerical operation order remain.
Two in-flight leases fit the three-window pool. Reuse is queued behind the prior
consumer on the same GPU stream, not merely behind NHI completion.

This remains an exclusive raw-message transport, not reliable RC. Used sessions
require module reloads on both peers. See [the ABI contract](../../docs/GPU_DIRECT.md).

## Matched transport diagnostic

Same executable on both hosts, eight warm-ups plus 256 measured exchanges at
each shape. Timing includes GPU production, RX prewarm, publish/wait, sum and
independent per-word GPU verification. It is neither one-way latency nor the
DMA-only timer in [the initial hardware report](2026-10-02-gpu-direct.md).
Bandwidth is decimal GB/s per direction during simultaneous bidirectional work.
Rounded fuzzy results (misty agrees closely):

| Payload | Verbs µs | Direct µs | Verbs GB/s | Direct GB/s |
| --- | ---: | ---: | ---: | ---: |
| 16 KiB | 43.4 | 52.8 | 0.377 | 0.310 |
| 1 MiB | 355.8 | 375.0 | 2.947 | 2.796 |
| 4 MiB | 1181.8 | 1351.3 | 3.549 | 3.104 |
| 32 MiB − 64 B | 8643.2 | 12807.0 | 3.882 | 2.620 |

At 1 MiB, process user + system CPU time per exchange is 964–967 µs direct
versus 710–711 µs verbs, approximately **36% more**. The corresponding average
occupancy is 2.57–2.58 versus about 2.0 CPU cores per process. This excludes
interrupt-handler and independent kernel-worker CPU. GPU utilization and
energy were **not measured**; no reduction in either is claimed.

The final direct cohort verifies **12,548,983,392 bytes per direction**, 2,112
exchanges including warm-ups and 3,168 DMA batches, with zero mismatches.
Shapes include 4 bytes, ragged tails, rotating dirty RX windows, and transfers
larger than the 8 MiB four-rail batch limit. A deliberate shape mismatch exits
cleanly on both ranks before TX, with zero payload bytes and no hanging GPU wait.

Earlier 128-run cohorts measured 1 MiB at 366 → 410 µs, CPU +40–42%. They remain
retained as historical results, not pooled with the final run. Making TX
cacheable improved some shapes. A fine-grained RX experiment passed correctness
but measured 389 µs at 1 MiB, 1292 µs at 4 MiB and 12871 µs near 32 MiB: no
consistent improvement. It was rejected; the retained adapter uses uncached RX.

## Model integration and the next step

The Q8 C2 smoke prefills two synthetic 2048-token prompts (4096 total). Both
ranks and transports produce the same full-logit hashes, tokens and active-member
transitions. Both prompts stop at EOS after 1 and 3 generated tokens, so this is
**not a tg128 benchmark**. It does not establish upstream accuracy, sampled/MTP,
serving concurrency, images, cancellation or snapshot compatibility.

| Diagnostic run | Prefill time, rank 0 | Total tokens/s |
| --- | ---: | ---: |
| Original verbs control | 4.157 s | 985.4 |
| Initial direct integration | 4.948 s | 827.8 |
| Direct with two-lane overlap | 3.941 s | 1039.2 |
| Fresh old-direct control after that run | 4.973 s | 823.7 |

The new schedule removes about **21% of old-direct prefill wall time** in this
focused diagnostic. The original verbs comparison is historical, not a claim
of a qualified gain over verbs. Model load is outside the timer, but each run
uses fresh executors without a separate shape warm-up. A warmed production
pp2048/tg128 comparison and GPU profiling remain required.

The source establishes several plausible costs: a system-release boundary per
partial, uncached GPU RX reads, TCP readiness and multiple ioctls per DMA batch,
and serialization between batches. The scheduling experiment directly supports
missing compute/transfer overlap as a cause of the original prefill regression.
It does not assign percentages to the other costs. Zero-copy removes payload
copies but does not automatically remove these synchronization costs.

## Longer prefill diagnostics

The next integration step also exercises two synthetic prompts of 32,768 or
65,536 tokens each. Both use context capacity 66,048, two members and an eight-token
decode budget. Only prefill is the timing target; the short decode verifies
continuation logits, membership and tokens. These are fresh-process diagnostic
runs, not warmed serving results or a tg128 benchmark.

At 32K per member, verbs takes **59.407 s (1103.2 tokens/s)** for 65,536 total
tokens; direct overlap takes **57.308 s (1143.6 tokens/s)**. Both ranks and
transports have identical complete logit/token signatures. The 3.5% shorter
prefill in this one comparison is small enough that repeatability is still
unqualified. Each rank exchanges 3,072 partials of 20,971,520 bytes during prefill.

The longer tests exposed a startup issue: misty's model load can finish over
30 seconds later. Two failed pre-prefill barrier logs are retained separately
(one ragged case and one 32K case). The adapter now gives only the synchronous
empty barrier a bounded 180-second receive allowance, then restores 30 seconds
before payload exchanges. No DMA or queued GPU wait is pending during this
barrier. DMA and data-control deadlines are unchanged. The successful 32K run
passes a load skew slightly above 30 seconds; model load remains outside timing.

## Reproduction and provenance

- Hosts: gfx1151 Radeon 8060S, ROCm 7.2.3, Fedora kernel
  `7.2.8-100.fc43.x86_64`, two USB4 cables/four rails.
- Gufo base: `c15a6234cfcd607a731eb666bd45298b83f0d298` plus the task changes.
  Historical 128-run transport cohorts used `6ddc7cc2` plus the initial adapter.
- Local Gufo was clean at `6ddc7cc2` and fast-forwarded to the existing `c15a6234`
  host base before applying the tested changes. No commit or push was made.
- CMake `gpu-tp2`, pinned Nix GCC 15.3 / HIP clang 22 / ROCm 7.2.3, targets
  `qwen38_flash_next_transport_probe`, `qwen38_flash_next_tp_batched_probe`, plus
  the affected `serve.cpp` object. This is not a complete production executable
  build. Added tp2 dev-shell dependencies were syntax checked; builds used the
  existing pinned environment with explicit Python and tools disabled.
- Test runtime uses a private mount namespace and ABI-compatible verbs libraries
  copied from `localhost/gufo-tp2-dev:7.2.3`; both transports use that same runtime.
  This does not change system libraries or the production image.
- Q8 model: `/opt/models/qwen3.8-flash-next/Q8_0/Qwen3.8-Flash-Next-Q8_0-00001-of-00006.gguf`.
- Final transport executable SHA256:
  `1ead1cdd778e551fac0fb0465df5182f4b61b638fe089a742d8564a082645dbd`.
- Original model comparison executable:
  `cab9c3b562ffecfaa910b63d89fc97d324c62c4d892ea099a6537455b2c8aef8`.
- Fresh old-direct control executable:
  `6d3e4ce606a5979d68fa94b0efb2735afa52f7caf1893c34a9e37c889e90d892`.
- First overlap smoke executable:
  `fe1fa9d9b51c670b36d201536fd4c311c226f4495e4069896c43dbdbe53d6b36`.
- Formatted overlap executable before the empty-barrier fix:
  `18957e737d0c00c22e36989ef7eab8f63c79e4208b298518deed4e0e930b653c`.

Transport arguments: `--rank 0|1 --transport verbs|donnerkeule --iterations 256`,
rank 1 adds `--connect 192.168.210.148`; verbs adds `--rdma-device usb4_rdma0`.
Model smoke arguments: `--tp-rank 0|1 --tp-transport verbs|donnerkeule --model PATH
--context 4096 --width 2 --prompt-tokens-0 2048 --prompt-tokens-1 2048 --max-tokens 128`;
rank 1 adds `--tp-bootstrap-host 192.168.210.148`; verbs adds
`--tp-rdma-device usb4_rdma0 --tp-gid-index 1`.

The 32K verbs control used the formatted overlap executable; the 32K direct
retry and subsequent tests use the same code with the untimed empty-barrier fix,
SHA256 `4c9aa6a3d4fbb235aaa0656b182d07ed7aeb63558cf01625a54bb4bc58b362bb`.

Logs and the executable consistency checker are in
[2026-10-03-gufo-direct/](2026-10-03-gufo-direct/). Run
`python3 bench/results/2026-10-03-gufo-direct/verify.py`.
The earlier kernel-WARN investigation and untested reset/unplug cases remain
listed in the initial hardware report; this model experiment does not close them.

## Final checkpoint

The 64K direct attempt did not reach prefill. Fuzzy's model-loading threads
waited in `iomap_dio_rw` on `/opt` (`/dev/nvme0n1p2`, ext4), with high I/O pressure
and slow read progress; NVMe state remained `live`. Misty timed out at the empty
barrier. The own test was terminated; the root storage cause is unproven.
Both failed logs are retained as `*-overlap-64k-load-stall.log`.

The probe now builds with `--prefill-sweep` to keep one model loaded across
multiple sizes and create fresh request sessions per case. No GPU sweep has
been run; earlier timings retain their original fresh-process provenance.
The modified module's normal verbs mode has not yet had the additional planned
compatibility run. Both hosts are idle with direct mode enabled and verbs
registration disabled; no old Gufo service was restarted.

See the [complete current status and architecture recommendation](../../docs/history/GPU_RDMA_STATUS_2026-10-04.md).
