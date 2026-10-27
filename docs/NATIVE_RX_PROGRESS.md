# Native RX progress using the polling contract

## Problem

On two AMD Ryzen AI Max+ 395 / 128 GB systems, native host-buffer exchanges
sometimes waited about 70 ms for an RC retry even though the receiver's DMA
descriptor was already COMPLETED. This primarily affected exchanges after
300µs requested idle, rather than sustained Gufo traffic.

Read-only IRQ/worker probes found 157 completed-tail worker exits followed
by more than 10 ms with neither another IRQ nor a worker entry on that ring.
One completed tail waited 75.6 ms. All six diagnostic captures were loss-free
with zero missed return probes. This establishes a notification/progress
failure; it does not establish a specific hardware interrupt defect or prove
that the interconnect lost a packet.

The normal NHI worker collects completed frames, releases its ring lock and
runs callbacks. More DMA can complete while callbacks repost buffers. This
change uses the kernel's existing interrupt mask/drain/rearm polling contract
instead of depending exclusively on another notification of the normal
callback worker.

## Implementation and options

`native_rx_progress=1` selects this path for native, non-DMA-BUF RX only.
It is opt-in; the default remains the existing receive path.

1. `tb_ring_alloc_rx()` receives a `start_poll` callback. The NHI masks the
   ring interrupt before invoking it.
2. The callback schedules one per-path delayed worker.
3. That worker calls `tb_ring_poll()` and invokes live callbacks in ring
   order. A 256-frame budget yields between batches while retaining the
   interrupt mask during burst draining.
4. Once drained, `tb_ring_poll_complete()` rearms the interrupt.

`native_rx_recheck_ms=1` is the default optional liveness recheck interval;
it uses that same consumer. Zero disables periodic work (IRQ-only).
Values above 1000 are rejected when the new mode is enabled. The recheck
adds wakeups while a native path exists, including while no QPs are active.

IRQ and timer enqueue both hold a per-path lock around the enabled check and
scheduling. Destruction clears enabled under that lock, then cancels the work
synchronously before stopping/freeing the ring. No ring API is called while
holding this scheduling lock. The normal NHI worker supplies canceled
callbacks after polling has been disabled; there is no second live RX
callback producer. The worker remains in process context like existing
verbs callbacks.

The production change uses exported ring APIs and no private descriptor
layout, MMIO register access, or alternate kernel-worker invocation. TX
polling and the wire protocol are unchanged. Apple and DMA-BUF retain their
existing paths.

Debugfs summary includes configuration plus IRQ, worker-run, frame, empty-run
and budget-exhaustion counters prefixed `native_rx_progress_`.

## Hardware results

Both machines used kernel 7.3.0-strix-test and one front-to-front USB4 cable
negotiated at 2×10 Gbit/s per direction, with two logical native rails.
The fixed driver options were:

```text
profile=linux_perf tbnet=prefer_rdma lanes=2
bind_services=1 allocate_rings=1 start_rings=1 negotiate_native=1
enable_tunnels=1 register_verbs=1 native_write_striping=1
```

The baseline driver was e0ab7b480f58fce2edfe2bc4042a2c4015b3ed85.
A custom reusable host-buffer WRITE_WITH_IMM request/reply harness recorded
completed exchange RTTs at 8/16/32/64 KiB, 500 observations per size and endpoint
role, following 300 µs requested idle. The table pools sizes and roles; roles
are related observations, not independent network transactions. Retry
counter deltas also include warmup, so they are not one-to-one slow RTTs.

| Mode | Median µs | p99 µs | >10ms observations /4,000 | Retries |
|---|---:|---:|---:|---:|
| Legacy control1 |80.468|73,728.099|272|184|
| New consumer, IRQ-only |80.832|202.773|0|0|
| New consumer +1ms recheck1 |89.622|290.882|0|0|
| Legacy control2 |76.523|72,826.873|231|172|
| New consumer +1ms recheck2 |93.022|208.443|0|0|

The IRQ-only result shows that these improvements do not require a periodic
watchdog. The repeated legacy control reproduced the stall. Additional cases
covered host exchanges with 0/10000 µs idle, Gufo host-window exchange shapes,
40 large shape/endpoint cases,5,000 observations per size during a longer
back-to-back run, peer exit and fresh reconnection. Both new-mode runs passed
payload, retry/error-counter, tail-latency and reconnection gates.

Hardware measurements used the same consumer with additional metadata-only
profiling hooks; hooks were disabled and probes unloaded for the timed runs.
Those experimental hooks/probes are omitted from this patch. The PR build
has been compiled and software-tested separately.

The summaries live under [bench/results/native-rx-progress](../bench/results/native-rx-progress/).
They are aggregate measurements, not the original per-iteration CSVs or
profiler traces. The custom hardware harness is not included in this focused
patch; existing `bench/sweep_perftest.py` supplies independent perftest
coverage, but does not reproduce this particular idle request/reply test.

## Validation and limits

```sh
make -C kernel -j8
python3 tools/ci/check-syntax.py
python3 tools/ci/run-smoke.py
python3 -m unittest discover -s tools/ci/framework-desktop-tests -p 'test_*.py'
TBV_TEST_SANITIZERS=1 python3 -m unittest discover \
  -s tools/ci/framework-desktop-tests -p 'test_native_rx_progress.py'
```

The C harness executes the actual production helpers: ordered 1025-frame
drain, budget continuation, an IRQ at rearm that must not be postponed by a
timer, IRQ-only behavior, disabled work, and 100,000 concurrent IRQ enqueue
versus stop attempts. Kernel concurrency and real IRQ behavior still require
hardware tests.

Gufo back-to-back 10 KiB exchange medians remained around 31 µs, and ordinary
exchange throughput showed no material improvement. These results fix
transport tail latency; they do not establish a model token-rate improvement.
Five-second whole-machine CPU samples were noisy and did not isolate the
periodic recheck's wakeup cost.

The test does not qualify depth 128, GPU DMA-BUF, Apple interoperation, cable
hot-unplug, all verbs opcodes or overnight reliability. The opt-in default
lets those configurations be tested before broader enablement.
