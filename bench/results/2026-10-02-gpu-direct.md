# HIP DMA-BUF → four NHI rails → HIP consumer

The requested GPU-to-GPU data path works on **fuzzy ↔ misty**, using their two
USB4 cables and four native Donnerkeule rails. The final retained tests completed
**6,272 bidirectional exchanges and 13,420,724,224 GPU-verified payload bytes**,
with zero mismatched words, descriptor errors or canceled data frames. Large
transfers reached approximately **4.1–4.2 GB/s in each direction concurrently**.

This proves an exclusive experimental message path, not Gufo model performance,
standard verbs DMA-BUF registration, reliable RC, or arbitrary remote-address
WRITE. See [the interface and Gufo integration contract](../../docs/GPU_DIRECT.md).

## Setup and what was actually verified

- Both hosts: AMD Radeon 8060S, `gfx1151`, Fedora kernel
  `7.2.8-100.fc43.x86_64`, HIP runtime `70253211` (ROCm 7.2.3).
- Two USB4 cables; each reports 20 Gb/s links with width `0x2`. Two NHI rings
  per cable form four rails. Physical link UUIDs and HopIDs pair the endpoints;
  host domain enumeration is reversed on the two machines.
- Dedicated `hipMalloc` TX/RX allocations, exported as DMA-BUFs and statically
  attached/mapped to every actual NHI device. Separate per-device IOVAs feed
  the descriptors. No CPU payload mapping, staging pool, or payload copy.
- Kernel A generates a different rank/sequence/index pattern every exchange.
  The peer's kernel B checks every 32-bit word directly in the imported RX
  allocation. The CPU reads only the 16-byte GPU mismatch/checksum result.
- Three rotating windows; RX is read on the GPU **before** every DMA to exercise
  cache reuse. Timing-enabled `hipEventReleaseToSystem` markers bracket the
  external DMA/dispatch boundaries. The RX acquire interpretation remains
  specific to this tested runtime; it is not a general HIP guarantee.
- TCP carries topology and fixed readiness/completion records only. It never
  carries tensor data. The GPU transport is bidirectional in every measured row.
- Base checkout: `e37ecaf709a6d82171a97791dcf6e9ebc92abf69` plus the working-tree
  implementation. Tests started October 2 and continued October 3, 2026.

## Four-rail results

`dma_us` covers local TX submission through both TX and RX completion, excluding
the preceding readiness exchange and subsequent GPU verification. Throughput is
decimal GB/s (`bytes / dma_us / 1000`), per direction. Numbers are cohort means,
not tail latency measurements. Initialization is outside the measurement; no
separate steady-state warm-up was excluded.

| Memory | Bytes | Exchanges | Fuzzy DMA µs | Misty DMA µs | Fuzzy GB/s | Misty GB/s |
|---|---:|---:|---:|---:|---:|---:|
| Coarse | 16,384 | 256 | 15.93 | 15.93 | 1.029 | 1.028 |
| Coarse | 65,536 | 256 | 27.80 | 31.47 | 2.358 | 2.082 |
| Coarse | 266,240 | 256 | 69.02 | 69.84 | 3.857 | 3.812 |
| Coarse | 1,048,576 | 256 | 247.97 | 249.85 | 4.229 | 4.197 |
| Coarse | 4,194,304 | 256 | 1,023.76 | 1,035.10 | 4.097 | 4.052 |
| Uncached | 16,384 | 128 | 23.22 | 21.67 | 0.705 | 0.756 |
| Uncached | 65,536 | 128 | 26.70 | 32.11 | 2.455 | 2.041 |
| Uncached | 266,240 | 128 | 70.68 | 72.19 | 3.767 | 3.688 |
| Uncached | 1,048,576 | 128 | 259.18 | 259.10 | 4.046 | 4.047 |
| Uncached | 4,194,304 | 128 | 1,006.53 | 1,011.53 | 4.167 | 4.147 |

The 260 KiB case deliberately produces uneven contiguous rail blocks:
17/16/16/16 frames. Both matrix runs had zero watchdog kicks.
At 4 MiB, the full verified exchange including GPU work and CPU control took
about 1.50 ms; this is distinct from the approximately 1.02 ms DMA interval.
Different cohort lengths and ordinary host scheduling preclude interpreting
small coarse/uncached differences as a cache-performance conclusion.

## Rail scaling and sustained reuse

All rows below use 1 MiB coarse transfers. The one- and two-rail cohorts use
one cable; four rails use both cables.

| Rails | Exchanges | Fuzzy DMA µs | Misty DMA µs | Fuzzy GB/s | Misty GB/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 128 | 647.94 | 648.11 | 1.618 | 1.618 |
| 2 | 128 | 414.54 | 413.19 | 2.530 | 2.538 |
| 4, matrix above | 256 | 247.97 | 249.85 | 4.229 | 4.197 |
| 4, longer run | 4,096 | 250.30 | 254.75 | 4.189 | 4.116 |

The longer run completed 1,048,576 TX and 1,048,576 RX descriptors **on each
host**, evenly split as 262,144 per rail per direction. Misty needed eight
worker requeues across two waits; fuzzy needed none. These waits succeeded
after the bounded completion rescan, with full GPU verification and no retry
or retransmission. The reported mean includes these delays.

Across all five final cohorts, each host records 1,638,272 TX submissions,
1,638,272 TX completions and 1,638,272 RX completions. The normal verbs-copy
counter stays zero. Source inspection also confirms no payload mapping/copy;
zero in an unused verbs counter alone would not establish that property.

## Failures found and handled

1. An earlier interrupt-only matrix stopped after 384 completed small exchanges
   and during the 58th 1 MiB exchange. Misty had 25,725 RX completions versus
   25,728 transmitted frames; abort canceled the remaining three descriptors.
   The failure logs are retained, not included in the passing totals. This
   resembles strix-rdma's documented missing-final-MSI-X issue. The exact IRQ
   cause was not proved here. `WAIT` now requeues the same NHI completion worker
   after 10 ms without batch completion, preserving the caller's deadline and
   requiring real descriptor completion. The final soak exercised this path.
2. The initial 3 MiB pool used for rail scaling was rejected at import because
   the exported BO was larger than the requested HIP range. The tester now
   rounds dedicated pools to 2 MiB; the importer's exact-object-size check stays
   strict. No descriptors were submitted by the rejected import.
3. Review found that session close stopped the leased rings, then ordinary path
   destruction stopped them again. Destruction now retains tunnel cleanup and
   skips rings already drained by the GPU session.
4. The no-sender test performs 18 negative checks per host, including partial
   import rollback, invalid geometry, duplicate operations and post-abort TX.
   With a 50 ms deadline, each host queued 16 worker rescans, reported **zero
   successful TX/RX completions**, canceled its one armed RX frame and returned
   `-ETIMEDOUT`. Rescanning does not manufacture success or hide a missing peer.

The sticky kernel taint value changed from 4096 to 4608 during early prototype
testing (the WARN bit). The double-stop above is a plausible explanation from
code inspection, but the exact warning record has **not** been verified:
automatic approval review blocked kernel-log access pending explicit permission.
No claim of a warning-free kernel is made, and no reboot was performed to clear
the flag. Physical unplug, peer reboot, GPU reset, IOMMU fault injection and
long-duration production stress remain untested.

## Build, artifacts and restoration

- Module build against the installed Fedora headers with `W=1`: passed; the
  existing `ibdev.c` device-name `snprintf` warning remains. The new importer
  produced no compiler warnings. Kernel checkpatch reports zero errors and
  six style warnings, including a preference for a newer allocation helper;
  `kzalloc` preserves the repository's Linux 6.14 baseline.
- HIP build with `-O2 -std=c++17 -Wall -Wextra -Werror`: passed.
- Local exhaustive pool geometry/SG/bounds/overflow smoke test with ASan/UBSan,
  existing protocol and reliability smoke tests, shell syntax and whitespace
  checks: passed.
- Test module SHA-256, before host-specific signing:
  `676a13a857f12cb75c6a5090f81d47b51ea22fed174af0bfdd52ab65979a58b7`.
- Final tester SHA-256:
  `176d926000b9dba979e64b2de92b8b29705307ccb42974bb4ef33978ea331ca0`.
  The final tester extends only the no-sender timeout from 10 to 50 ms after
  the data cohorts; its transfer path is unchanged. The first coarse matrix
  preceded the pool-rounding adjustment; its 12 MiB allocation was already
  a multiple of 2 MiB.

Raw test output, per-rail counters and full summaries are retained under
[`2026-10-02-gpu-direct/`](https://github.com/neuhaus/donnerkeule-results/tree/main/2026-10-02-gpu-direct/), separately for each host.
An earlier successful interrupt-only matrix and the subsequent failed matrix
are retained as historical evidence, excluded from final cohort totals.
Run the independent retained-results check with:

```sh
python3 bench/results/2026-10-02-gpu-direct/verify.py
```

Both machines were restored to the exact original signed module, Build ID
`3d170497e0e99f3c49a81b13b0819530b5a3c0e0`; every saved module parameter was
compared successfully. Installed module files were not replaced. The experimental
device is gone and all four normal verbs rails are ready. Gufo's original
`gufo@q8.service` and `gufo-rank1@q8.service` are active. The coordinator reports
`{"status":"ok"}` from `/health` and
`{"status":"ready","model":"qwen3.8-flash-next"}` from `/ready`.

Gufo source and production configuration were not changed. Its next integration
step is a communicator that lends the producer its actual DMA-BUF output window
before kernel launch and keeps the peer window leased until consumer completion.
It also needs a qualified RX acquire for queued GPU work and generation-aware
reconnect/retry before replacing reliable verbs operation.
