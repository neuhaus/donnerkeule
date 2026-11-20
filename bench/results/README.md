# Measurement archives

Every qualification run keeps its Markdown report here. The raw logs and
reproduction artefacts live in
[neuhaus/donnerkeule-results](https://github.com/neuhaus/donnerkeule-results)
with full commit history preserved; each report links directly to its raw
evidence there.

## Reports, newest first

| Date | Report | Subject |
| --- | --- | --- |
| 2026-10-25 | [donnerkeule-merge](2026-10-25-donnerkeule-merge.md) | Donnerkeule fork changes on the Sixunited AXB35 |
| 2026-10-18 | [verbs-loss](2026-10-18-verbs-loss.md) | Loss and reconnect qualification of direct GPU verbs with SEND credits |
| 2026-10-16 | [verbs-credits](2026-10-16-verbs-credits.md) | Pre-armed SEND credits for direct GPU verbs |
| 2026-10-16 | [verbs-parity](2026-10-16-verbs-parity.md) | Direct GPU memory through verbs: driver results |
| 2026-10-08 | [hardware](2026-10-08-hardware.md) | Hardware check: DONE, polling and refactoring |
| 2026-10-07 | [xdomain-done](2026-10-07-xdomain-done.md) | XDomain DONE: local implementation state |
| 2026-10-07 | [polling](2026-10-07-polling.md) | Less polling: local state |
| 2026-10-06 | [verbs-control-ring](2026-10-06-verbs-control-ring.md) | NHI control channel behind DMA-BUF verbs |
| 2026-10-05 | [verbs-dmabuf-optimization](2026-10-05-verbs-dmabuf-optimization.md) | Verbs DMA-BUF: profiling and first optimization |
| 2026-10-05 | [verbs-dmabuf](2026-10-05-verbs-dmabuf.md) | Verbs DMA-BUF: first hardware qualification |
| 2026-10-03 | [gufo-direct](2026-10-03-gufo-direct.md) | Gufo direct DMA-BUF integration |
| 2026-10-02 | [gpu-direct](2026-10-02-gpu-direct.md) | HIP DMA-BUF to four NHI rails to HIP consumer |

The reports from 5 to 8 October are written in German.

Two runs keep a second README beside their evidence:
[2026-10-08-hardware/](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-08-hardware/README.md) and
[2026-10-16-verbs-parity/](https://github.com/neuhaus/donnerkeule-results/blob/main/2026-10-16-verbs-parity/README.md).

## perftest sweeps by host pair

Named for the hardware rather than the date, each with its own `perftest.md`:

- [8845HS-mbp-1x40g](https://github.com/neuhaus/donnerkeule-results/blob/main/8845HS-mbp-1x40g/perftest.md)
- [strix-noiommu-mbp-1x40g](https://github.com/neuhaus/donnerkeule-results/blob/main/strix-noiommu-mbp-1x40g/perftest.md)
- [strix-2p-noiommu-2x40g](https://github.com/neuhaus/donnerkeule-results/blob/main/strix-2p-noiommu-2x40g/perftest.md)

## Recreating a run

Each report names the command that produced it, and each run directory in
[neuhaus/donnerkeule-results](https://github.com/neuhaus/donnerkeule-results)
keeps the scripts and `.parameters` files used on the day.
