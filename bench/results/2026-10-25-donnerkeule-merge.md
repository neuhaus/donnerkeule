# Donnerkeule fork changes on the Sixunited AXB35, 25 October 2026

The [donnerkeule fork of lfthht](https://github.com/lfthht/donnerkeule/tree/codex/framework-desktop)
added 22 commits on top of `e4039b6`, measured on two Framework Desktops. This
merges its kernel, provider and test changes (branch `donnerkeule-merge`) and
qualifies them on fuzzy and misty: Bosgame M5 (Sixunited AXB35-02), Fedora 43,
kernel 7.2.11, two cables, four rails, `amd_iommu=off`. Its DeepSeek engine
patches, benchmark artifacts and Framework-specific tooling are not merged.

## What was merged

- Opt-in completion fallback timer (`dmabuf_poll_us`, `dmabuf_poll_control`),
  its ownership fix and ready-descriptor check.
- Opt-in batched doorbell for AMD 158d/158e NHIs (`dmabuf_batch_doorbell`),
  configurable post burst, coalesced host copies.
- Bounded QP drain on normal close; per-rail descriptor/staging reuse
  (`dmabuf_reuse_buffers`) with its nested-credit fix; quarantine reason in
  `peers` debugfs.
- Direct DMA for aligned pinned host MRs (`dmabuf_host_direct`,
  `dmabuf_host_direct_min_bytes`) and host receive credits (`dmabuf_host_credits`).
- No MAD/CM capability in DMA-BUF mode (removes the per-rail `Couldn't create
  ib_mad QP1` messages, which fuzzy logged too).
- Provider CQ capacity fix and the fork's production-C regression tests.

Changes made here:

1. **Doorbell ABI guard.** The helper compiled only on exactly 7.2.6, so on
   7.2.11 it was a no-op; its build-time layout checks would also have broken
   builds on other kernels. It now builds on x86_64 Linux 7.2.x, with
   `struct tb_ring` still checked at build time (same layout on 7.2.11).
2. **Credit race (pre-existing in this tree).** With doorbell and timer on,
   the SEND size matrix failed in 3 of 9 runs: `unexpected credit release
   1102 (armed 1103 state 4)`, then all rails stopped. Counters showed the
   receiver had *adopted* that credit for a PREPARE, while the sender later
   filled and released it. Cause: when the adopting PREPARE's exchange timed
   out, the retried PREPARE got a plain READY without the credit. The
   receiver now repeats the adopted credit in every READY for the request
   (`credit_readopted` counts it). Afterwards 0 mismatches in every run; the
   counter showed 0–6 such retries per run, each of which would have stopped
   the connection before.
3. **Timer lock contention.** With `dmabuf_poll_control=1` each 25 µs tick
   took `state->lock` and `ring_lock`, so WRs queued behind it: 10,304 B
   SEND_IMM 106 µs instead of 89 µs, Gufo q4 at 2/4 requests 10%/7% slower.
   Ticks now skip when a lock is held: 87.5 µs, Gufo back to the default
   level, 363 of 387,090 ticks skipped.

## Correctness

Both profiles below passed on the final build with full payload and guard
checks: SEND, SEND_IMM, WRITE_IMM and WRITE over 11 sizes (depth 3, two rounds
with QP/MR recreation), remote-access error, 100 ms delayed receive, host-MR
variants of all four ops (direct host DMA used: `dmabuf_host_maps` grew),
2 × 4000 persistent SEND_IMM, and timed `ib_write_bw` runs (QP destroy with a
full SQ, which the drain now covers). No stopped rail, credit mismatch or
abort, and no BUG/WARNING in either kernel log; all four rails
`descriptor_ready=1 quarantine_reason=0` afterwards. Local smoke suite passes.

| Profile | Cases | 16 MiB host WRITE (3 runs) |
|---|---|---|
| AXB35: doorbell, burst 128, coalesce, reuse, host direct/credits, 4 KiB | 11/11 | 38.2, 40.8, 38.4 Gb/s |
| Framework: the same plus `dmabuf_poll_us=25 dmabuf_poll_control=1` | 11/11 | 42.8, 42.5, 43.0 Gb/s |

## Transport measurements

One configuration change at a time from the Framework profile, before the
trylock change (SEND_IMM: GPU verifier, 10,304 B persistent, µs per checked
message; allreduce: Gufo `tp_batched_probe --allreduce-bench 1000`, p50 µs,
rank 0 / rank 1; all sums verified):

| Configuration | Host WRITE | SEND_IMM | 1 row | 8 rows | 512 rows (5 MiB) |
|---|---:|---:|---:|---:|---:|
| options off | 20.1 Gb/s | 89.2 | 31.7 / 32.1 | 46.1 / 45.6 | 1309 / 1302 |
| Framework profile | 43.0 | 105.9 | 31.0 / 32.1 | 41.0 / 48.1 | 1016 / 1174 |
| − doorbell | 35.7 | 94.9 | 32.9 / 33.4 | 48.6 / 48.9 | 1372 / 1373 |
| − timer | 39.0 | 92.4 | 32.1 / 32.6 | 45.0 / 45.1 | 1116 / 1128 |
| − control-ring timer | 45.0 | 92.1 | 31.4 / 31.9 | 45.5 / 43.9 | 1100 / 1094 |
| − buffer reuse | 42.5 | 95.8 | 32.2 / 33.7 | 39.5 / 46.6 | 1088 / 1126 |
| − host direct/credits | 27.1 | 95.2 | 32.2 / 32.8 | 44.6 / 44.2 | 1108 / 1092 |
| Framework profile, trylock build | 45.6 | 88.3, 87.5, 87.5 | 30.9 / 30.1 | 48.4 / 40.8 | 1125 / 1076 |

The doorbell drives the large-exchange gain (−15% at 512 rows, p99 about
halved); direct host DMA plus the doorbell double host-memory bandwidth.
Small exchanges do not change.

## Gufo

Qwen3.8-Flash-Next, TP=2, `windows=gpu_send` (GPU SEND_IMM with credits, so the
host-DMA options do not apply), `gufo-serving-bench` with 256 output tokens,
5 repetitions, one warmup, 473 prompt tokens. The production unit uses MTP
speculative decoding; its acceptance varies between runs (0.52–0.66), which
moves decode tok/s by ±15% independent of transport (q8 production runs:
60.8 and 46.5 tok/s at C=1). The tables therefore use runs without MTP (same
image and checkout, started by hand). Values are medians of samples, decode
tok/s; prefill in tok/s.

q4 (UD-Q4_K_XL), runs alternated:

| Module and options | C=1 | C=2 | C=4 | Prefill C=1 |
|---|---:|---:|---:|---:|
| new build, options off (2 runs) | 36.18, 36.59 | 30.20, 30.55 | 23.62, 23.66 | 1076, 1081 |
| AXB35 profile (2) | 37.23, 36.21 | 30.04, 29.87 | 23.49, 23.22 | 1087, 1080 |
| Framework profile, before trylock (2) | 36.33, 36.51 | 29.93, **27.33** | 23.11, **21.96** | 1074, 1081 |
| Framework profile, trylock (2) | 36.45, 37.45 | 29.64, 29.90 | 23.05, 23.23 | 1080, 1079 |

q8 (Q8_0), runs alternated:

| Module and options | C=1 | C=2 | C=4 | Prefill C=1 |
|---|---:|---:|---:|---:|
| production DKMS 0.4.0 (4 runs) | 31.06, 33.38, 32.73, 29.61 | 24.97, 24.85, 25.52, 22.80 | 19.25, 19.65, 19.86, 18.77 | 704–743 |
| new build, options off (2) | 32.89, 31.71 | 25.17, 24.54 | 18.40, 19.60 | 742, 746 |
| AXB35 profile (2) | 32.78, 32.72 | 24.92, 25.29 | 19.49, 19.83 | 745, 745 |
| Framework profile, trylock (4) | 33.07, 29.57, 32.37, 33.11 | 23.67, 22.56, 24.56, 25.17 | 18.75, 19.13, 19.17, 19.43 | 709–746 |

**Result:** end to end, Gufo on the AXB35 is unchanged within run-to-run
variation (about ±3% for q8, ±1% for q4) by any of the merged options. The
AXB35 profile was at or above the production median everywhere and had the smallest
spread, so it is installed. The transport gains show up in host-memory
bandwidth and large exchanges, which Gufo's decode does not exercise.

## perftest sweep: native vs DMA-BUF verbs

The README charts were redrawn from two `sweep_perftest.py` runs (fuzzy
client, misty server, host memory, 1 QP): the installed module with
`dmabuf_verbs=0` (native, write striping) and with the AXB35 DMA-BUF profile.
Native reproduces the 30 September curves within a few percent. Selected
values (Gb/s or median µs; busy cores client/server in parentheses):

| Test | Size | Native | DMA-BUF |
|---|---:|---:|---:|
| ib_write_bw | 4 KiB | 9.11 (2.3/1.2) | 0.83 (1.4/0.3) |
| ib_write_bw | 256 KiB | 41.9 (4.6/4.1) | 21.7 (1.3/0.3) |
| ib_write_bw | 4 MiB | 43.2 (4.3/2.7) | 42.9 (1.3/0.4) |
| ib_write_bw bidirectional | 4 MiB | 83.0 (7.7/8.5) | 82.4 (1.5/1.6) |
| ib_send_bw | 256 KiB | 27.1 (2.5/4.9) | 23.2 (1.3/1.3) |
| ib_send_bw | 4 MiB | fails | 38.8 (1.3/1.3) |
| ib_read_bw | 256 KiB | 9.69 | not supported |
| ib_write_lat | 64 B | 7.15 | 29.8 |
| ib_send_lat | 64 B | 10.2 | 31.3 |
| ib_send_lat | 256 KiB | 104.1 | 65.5 |

Native keeps the lower small-message latency, the higher bandwidth below
4 MiB and RDMA READ; DMA-BUF needs far fewer cores and handles large SENDs.
Native `ib_read_lat` is flaky (16 KiB and 256 KiB failed in one run, 1 KiB,
16 KiB, 64 KiB and 256 KiB in the next) and native SENDs above 512 KiB fail
with `native SEND reorder store failed`. Native runs log rate-limited
retransmits that all recover; DMA-BUF runs were clean with 0 credit
mismatches. CSVs: [sweep/](https://github.com/neuhaus/donnerkeule-results/tree/main/2026-10-25-donnerkeule-merge/sweep/) (`charts.csv`
is what `plot_perftest.py` drew; `*-big.csv` are the uncapped checks).

## Installed state

Both hosts run DKMS `thunderbolt-ibverbs/0.4.0.dk1` (this branch) with the
AXB35 options appended to `/etc/modprobe.d/thunderbolt-ibverbs.conf`; the
0.4.0 source stays in `/usr/src` and `scripts/rollback.sh` restores it and the
saved options. Both hosts were rebooted and came up with the module, all options and
four healthy rails without manual steps. (The first reboot still had the old
options: the DKMS post-transaction dracut had copied `modprobe.d` into the
initramfs before the options were appended. `install.sh` now appends them
first; `rollback.sh` regenerates the initramfs.)

Final runs through the production units after the reboot (MTP on, so within
the noise described above): q8 42.2 / 34.1 / 23.1 decode tok/s at C=1/2/4,
prefill 731; q4 58.7 / 48.5 / 30.6, prefill 1040. 3.0 million bulk doorbell
calls, no credit mismatch, abort or stopped rail.

## Not covered

- Framework Desktop hardware (only the fork's own qualification).
- The loss/fault-injection set of [October 18](2026-10-18-verbs-loss.md),
  IOMMU on, physical cable pulls.
- The rebuilt userspace provider: Gufo keeps `/home/sn/tbv/c50`; the CQ
  capacity fix only matters for kernels that leave `cqe` zero, which this one
  does not.
- Why PREPARE exchanges occasionally time out (the cause of the retries in
  item 2); each costs at least 10 ms.

## Evidence

Scripts, verifier/probe/perftest logs and debugfs snapshots before and after
every case are in [2026-10-25-donnerkeule-merge/](https://github.com/neuhaus/donnerkeule-results/tree/main/2026-10-25-donnerkeule-merge/);
Gufo serving-bench JSON in its `gufo/` folder.
