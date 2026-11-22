# TODO

## DMA-BUF small-message performance

Observed 22 Nov 2026 (dk14, 7.3.4, H2H on): DMA-BUF is far slower than native
below ~256 KiB. 4 KiB WRITE ≈ 0.85 Gb/s (~26k msg/s ≈ one message per ~38 µs,
i.e. about one latency per message, so apparently no overlap); native 9–30 Gb/s
at the same sizes. DMA-BUF ib_send_lat is ~13–15 µs at 4 KiB but ~30–33 µs at
64 B–1 KiB; 4096 = dmabuf_host_direct_min_bytes, so sub-4 KiB likely takes a
slower path.

0. First trace one 64 B DMA-BUF ib_send_lat (tbv_perf + tb_tx/tb_rx on both
   hosts) to see where the ~30 µs goes.
1. Try dmabuf_host_direct_min_bytes=64 or 0 (TBV_DEV_PARAMS build) and rerun
   the 64 B–1 KiB latency cases.
2. Check whether dmabuf_batch_doorbell=auto delays single posts; add a
   flush-on-idle rule if so.
3. Find what limits overlap for small transfers (credits, push-done,
   one-in-flight per QP) and allow several outstanding descriptors.
4. Coalesce consecutive small WRITEs/SENDs into one DMA transfer
   (check what dmabuf_coalesce_copy already does).
5. Evaluate dmabuf_poll_us / dmabuf_poll_control for small messages.
6. Larger option: hybrid per-QP path, native framing below a threshold
   (~64–256 KiB), DMA-BUF above.

## Elsewhere

- Send the thunderbolt fixes upstream
  ([tools/thunderbolt-ringfix](tools/thunderbolt-ringfix/README.md)): 0001 + 0002
  as the fix series, 0003 as a follow-up.
- Native `ib_write_bw` at 16 KiB fails now and then (RNR retries, `reason=2`);
  native `ib_read_bw` at 16 MiB failed once
  ([bench/results/2026-11-22-usb4-h2h.md](bench/results/2026-11-22-usb4-h2h.md)).
