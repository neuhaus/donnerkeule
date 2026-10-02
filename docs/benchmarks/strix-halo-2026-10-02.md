# Strix Halo CQ measurements, 2026-10-02

Hosts: fuzzy and misty, Ryzen AI MAX+ 395 (16 cores / 32 logical CPUs),
Linux 7.2.8-100.fc43.x86_64, EC balanced power mode. Two USB4 cables,
10 Gbit/s × 2 lanes per cable, four active RDMA paths. Native WRITE striping,
512 maximum in-flight TX frames per path. The test module is loaded temporarily;
the installed DKMS module is unchanged. No inference performance claim is made.

## Completion polling

Both modes use the same corrected kernel and provider. `TBV_CQ_FORCE_IOCTL=1`
selects generic polling for the comparison. The test is `rc_write_imm_verify`,
with payload and completion byte-length validation; all runs below had zero
receiver errors. The peer acknowledges each message over TCP, so these results
do not represent a throughput limit for small RDMA transfers or inference.

| Fixed 10 KiB, 2 seconds under strace | Messages | ioctls | ioctls/message |
| --- | ---: | ---: | ---: |
| Generic CQ polling | 45,809 | 91,655 | 2.0008 |
| Mapped CQ polling | 54,537 | 54,553 | 1.0003 |

Completion polling removes about 50% of ioctls per message in this test.
Setup contributes the remaining small excess. The send path still uses a
system call. Traced timing is not used as a throughput comparison.

| Untraced variable-size 0–4 MiB, 6 seconds | Messages | Gbit/s | Sender user CPU s | Sender system CPU s |
| --- | ---: | ---: | ---: | ---: |
| Generic CQ polling | 18,844 | 26.55 | 2.15 | 3.84 |
| Mapped CQ polling | 19,662 | 27.74 | 2.92 | 3.06 |

The benchmark busy-polls, so reduced system time does not imply reduced total
CPU time. Short runs varied; the table does not establish a stable throughput
improvement. An untraced fixed-size 3,565,158-byte run transferred 10,506 messages
(37,455,549,948 bytes) in 8 seconds at 37.45 Gbit/s, with zero errors. A second
run at that size also reported 37.45 Gbit/s.

## CPU distribution

The JSON records per-CPU /proc/stat deltas across the complete 9.41-second
measurement window, including setup and shutdown around the 8-second transfer.
Busy percentages exclude idle and iowait. They are logical-CPU percentages,
not frequency-weighted work or physical-core saturation.

| Host | Total busy CPU equivalent | Share on four busiest logical CPUs | Busiest logical CPUs |
| --- | ---: | ---: | --- |
| fuzzy | 3.26 | 60.1% | 22, 30, 28, 10 |
| misty | 2.50 | 69.3% | 23, 26, 28, 30 |

Load is concentrated in the benchmark thread, per-ring interrupt handling and
kernel workers. IRQ balancing changes the CPU assignments between runs; these
CPU numbers are observations, not a proposed affinity configuration. CPUs
0–7 and 16–23 share one L3 cache; 8–15 and 24–31 share the other. The data does
not justify spreading work across all CPUs or selecting a permanent affinity.

## CQ correctness and packaging

`userspace/tests/cq_mmap_safety.c` passed on both hosts: capacities 1, 3, 32 and
4096, forged shared metadata, invalid consumer progress, retained mappings after
CQ destruction, and polling after an EBUSY destroy. Two simultaneous pollers
read 4,088 flush completions exactly once through a 513-entry ring, exercising
non-power-of-two index wraparound. This does not test 2^32 total-counter wrap.
The private index design handles that case without modulo arithmetic on totals.

All three generated packaging patches apply to clean rdma-core 58.0; the
resulting provider files are byte-identical to the edited provider source.
The kernel and provider were built and exercised on both hosts.
