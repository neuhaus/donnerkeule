# Native RX progress qualification summaries

See [the design and methodology](../../../docs/NATIVE_RX_PROGRESS.md).

- `pooled-host-idle300.csv`: completed host exchange RTTs pooled across
 8/16/32/64KiB and both endpoint roles;4,000 observations per phase.
- `per-size-summary.csv`: per-role/per-size host and Gufo statistics. Gufo
 excludes the first 100 observations of each 500-observation shape as warmup;
 host rows contain the requested recorded sample count after its warmup.
- `counter-deltas.csv`: full-case before/after counters, including warmup.
 Role0 and role1 identify the two endpoint roles. Counters count events,
 not independent application transactions.

These files contain aggregate summaries, not raw per-iteration data or trace
captures. Requested idle is a host sleep; it is not an exact measured wire gap.
