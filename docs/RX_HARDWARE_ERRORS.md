# Reject damaged receive descriptors before payload processing

The RX callback previously did not check `RING_DESC_CRC_ERROR` or
`RING_DESC_BUFFER_OVERRUN`. A descriptor can report an error while carrying a
valid-looking native header, allowing bad payload to reach completion handling.
The stock Linux Thunderbolt network receiver rejects those flags first.

Check before parsing/copying/ACKing, count the hardware errors and repost the
buffer normally. Do not trust a corrupt header for credits; a later valid framed
path sequence recovers a gap within the existing credit window. An already-active
headerless RAW stream fails closed until path recreation to avoid shifting later
chunks or falsely completing corrupted data. Automatic RAW recovery, lost RAW
headers, broader Apple/RAW qualification and physical CRC origin remain open.
No new setting, wire change, polling change or system installation is required.

A two-Strix-Halo fixed-buffer reproduction without GPU/model observed short
3840/3584-byte frames declaring4048payloadbytes, plus a4096-byte descriptor with
CRC+overrun followed by a completed payload mismatch. An integrated downstream
prototype with this RX gate and a separate downstream-only retry-cleanup correction
passed8,19240MiB+64-byte transfers (~320GiB), including5CRC drops/3retries. A second
fork-main build passed another8,192 transfers including2CRC drops/1retry, followed
by6matching Gufo TP2 request outputs/cache identities. Those are downstream
integration results, **not live-hardware qualification of this standalone upstream
port**. The downstream ordered-send QP-poisoning bug does not exist in this base
and is intentionally absent from this patch.

This exact upstream-base port passes the CPU smoke suite and module build.
The new tests compile the actual RX/sequence/native-frame callbacks under
ASan/UBSan and cover error rejection before payload, credit-gap recovery, repost,
cancellation, malformed length and active RAW fail-closed behavior. They do not
emulate USB4 hardware. No throughput/latency gain or overnight reliability is claimed.

```
python3 tools/ci/rx-hardware-errors-smoke.py
python3 tools/ci/run-smoke.py
make -C kernel -j6
```

`tools/tbv-host-integrity.cpp` is the fixed registered host-buffer reproducer.
Use the compatible provider environment, select `STRIX_RDMA_DEVICE` when needed,
and capture both logs plus before/after driver counters. This native test uses
GIDindex1. It includes CPU validation and management-network reuse fences, so it
is not a pure transport latency measurement.

```
c++ -std=c++20 -O2 -pthread tools/tbv-host-integrity.cpp -libverbs -o tbv-host-integrity
./tbv-host-integrity --self-test
# Receiver and sender use the receiver's actual management IPv4 address:
timeout 180 ./tbv-host-integrity 0 RECEIVER_IPV4 28781 1 41943104 4096 disjoint
timeout 180 ./tbv-host-integrity 1 RECEIVER_IPV4 28781 1 41943104 4096 disjoint
```

Use private rdma-core include/library/provider paths if required. Reverse roles
for the other direction. Every word, completion identity/order and safe buffer
reuse must verify; retain error/retry counts even when recovery succeeds.
