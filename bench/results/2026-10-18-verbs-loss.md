# Loss and reconnect qualification of direct GPU verbs with SEND credits, 18 October 2026

This follows up [the credits report](2026-10-16-verbs-credits.md), whose
"Status and next steps" listed loss and reconnect as uncovered. It covers the
opt-in direct mode `dmabuf_verbs=1 dmabuf_push_done=1 dmabuf_credits=1 lanes=2`
on fuzzy (rank 0) and misty (rank 1), Fedora 7.2.10, two cables (four rails).
Defaults are unchanged. This is a qualification of failure behavior, not a
performance measurement. Gufo was stopped for the whole session and was not
restarted afterwards at the user's request.

## Method

**Fault injection** (new, test only): module parameter `dmabuf_fault`,
`<credit|cancel|commit|release|done> <drop|fail|delay|dup> [count=N] [skip=N] [ms=N]`
or `clear`. It acts on the pushed XDomain notification this host transmits.
`cancel` is a CREDIT with a negative status. `drop` is silent loss, `fail`
returns -EIO to the caller as a failed `tb_xdomain_response()`, `delay` sends
once after `ms`, `dup` sends twice (the second copy after `ms`). It is a module
parameter because lockdown hides writable debugfs files. Idle cost is one atomic
read per notification. `peers` now shows `gpu_stopped=` per rail, and stopping a
rail is now logged.

**Workloads** (GPU verifier `tbv_gpu_verbs`):
- *W1:* `--op SEND_IMM --bytes 10304 --depth 1 --iterations 128 --persistent-rq
  --spin-us 5000` (credit stream, no size change).
- *Matrix:* `--op SEND --iterations 8 --rounds 2` over 11 sizes (size changes
  exercise CANCEL/RELEASE) and the full matrix with `--remote-error`.
- *Hold:* new `--hold-ms N --hold-rank R`. One more SEND of the tested size makes
  the holder re-arm a credit, and its receive WQE then stays posted while the
  rank waits. QP destroy, SIGKILL or a peer module reload are done in that window.
- *Kill:* 10,304 B, 256 KiB and 1 MiB streams, SIGKILL of one verifier at a chosen time.
- *Gufo probe:* `tp_batched_probe --allreduce-bench 500`.
- *Link loss:* `echo 1-2.0` and `1-2.1` into the `thunderbolt_ibverbs` driver's
  `unbind` (and later `bind`) on one host. This is the method of the
  2026-10-08 report. It removes the services and rings of one cable and the
  driver's peer. No cable was pulled.

After each quarantine both modules were reloaded (`lab.sh load dmabuf 1`).
Counters are deltas of the debugfs `summary` and `peers`. Raw output, kernel log
excerpts, fault hit counts and scripts are in
[2026-10-18-verbs-loss/](https://github.com/neuhaus/donnerkeule-results/tree/main/2026-10-18-verbs-loss/). A case name below is the file
prefix there (`<host>-<case>.log`, `<case>.case`). Hit counts in `.faults` are
cumulative since module load.

## Expected and observed

"Quarantine" means rails stopped on both hosts (`gpu_stopped=1`), WRs fail, and
the connection works again only after both modules are reloaded. All cases that
passed kept data correct (full payload and guard checks).

| Case (file prefix) | Expected from the code | Observed | Result |
|---|---|---|---|
| Lost DONE, 5 and all (`done-drop5`, `done-dropall`) | STATUS fallback after 10 ms | 261+256 DONEs dropped, all WRs OK; 5 s became 10 s | pass |
| Failed DONE send (`done-fail`) | Counted `notify_failed`, STATUS | pass | pass |
| DONE delayed 50 ms, duplicated, duplicated 100 ms late (`done-delay50`, `done-dup*`) | Late copies unmatched | pass, `control_unmatched` counts them | pass |
| Lost CREDIT, 3 and all (`credit-drop3`, `credit-dropall`) | Next PREPARE adopts the armed credit | adopted, no quarantine | pass |
| Lost CREDIT with size changes (`credit-dropall-matrix`) | CANCEL carries the whole credit, sender can release from it | 14 cancels, 13 returns, pass | pass |
| CREDIT delayed 20 and 200 ms; duplicated; duplicated 100 ms late | Late/duplicate copy is stale | `credit_stale` counts, pass | pass |
| Lost CANCEL, all (`cancel-dropall-matrix`) | The mismatched PREPARE's BUSY makes the sender release | 6 dropped, pass | pass |
| CREDIT + CANCEL both lost (`credit-cancel-dropall-matrix`) | Sender never releases; receiver stops rails after 500 ms | `credit_aborted`, 4+4 rails stopped, flushed CQE and a 30 s CQ timeout | fail as designed |
| Lost RELEASE, all (`release-dropall-matrix`) | Receiver stops rails after 500 ms | same, rails stopped | fail as designed |
| RELEASE delayed 300 ms / 700 ms (`release-delay300`, `release-delay700`) | OK below 500 ms; quarantine above, and the late RELEASE is a mismatch | pass / quarantine, kernel log "unexpected credit release" | pass / fail as designed |
| Failed CREDIT, CANCEL, RELEASE send (`credit-fail1`, `cancel-fail1`, `release-fail1`) | Arming without CREDIT, or a failed cancel, aborts; no RELEASE means timeout | quarantine in all three | fail as designed |
| Lost COMMIT (`commit-drop1`, `commit-drop1-fixed`) | Frames land in armed pages, never claimed, so the WR fails and rails stop | quarantine. Before the fix: immediate flush; after: sender "transport retry counter exceeded" after about 8.6 s, peer CQ timeout at 30 s | fail as designed |
| Failed COMMIT send (`commit-fail1*`) | WR fails | quarantine | fail as designed |
| COMMIT delayed 5 ms (`commit-delay5`) | OK | pass | pass |
| COMMIT delayed 20 / 50 ms, **before the fix** (`commit-delay20`, `commit-delay50`) | A late COMMIT should just arrive late | **Quarantine.** STATUS after the 10 ms DONE wait reached a receiver holding the credit speculative, got -ENOENT, the sender aborted and stopped its rails | **bug** |
| COMMIT delayed 20, 50, 200, 2000 ms, **after the fix** (`commit-delay*-fixed`) | STATUS answers BUSY while the matching credit awaits COMMIT | all pass | pass |
| Duplicated COMMIT, duplicated RELEASE (`commit-dup1*`, `release-dup1`) | The second copy finds no armed credit: mismatch stops the lanes | `credit_mismatch+1`, quarantine | fail as designed |
| Receiver QP destroy with armed credit (`hold-qpdestroy`) | CANCEL, sender fills and sends RELEASE | `canceled+1`, `returned+1`, pass | pass |
| Same with CANCEL lost (`hold-qpdestroy-cancel-lost`) | 600 ms wait, then drop with abort | `credit_aborted+1`, 4 rails stopped, nothing in dmesg (before the log was added) | fail as designed |
| SIGKILL of the holder with armed credit (`kill-holder`) | QP teardown as above | cancel, release, no quarantine, 0.9 s | pass |
| Peer module reload with armed credit, both directions (`unload-sender-while-armed`, `-r1`) | Disconnect aborts the armed credit; new peers get a new generation | `credit_aborted+1` on the armed side, new rails; next W1 passed without reloading the surviving host | pass |
| Holder SIGKILL plus immediate reload of its host (`kill-holder-and-reload-receiver`) | Same | pass, peers ready after 3 s; next W1 passed | pass |
| Random SIGKILL mid-stream, 9 trials (`kill-mid1..8`, first attempt discarded) | Clean flush or timeout; an in-flight batch stops the rails | 7 clean (survivor flushed within about 1 s), 1 survivor waited 30 s with no error, **1 quarantined all 8 rails** (256 KiB, killed in a transfer) | see below |
| Sender killed exactly between COMMIT and frames | Not reproducible | Direct posting sends COMMIT and frames in one syscall, so SIGKILL cannot land between them. Approximated by the random kills | skipped (not deterministic) |
| Link loss mid-stream (`linkloss-unbind-domain1`) | Services removed, in-flight batch aborts, rails stop | both ranks "Work Request Flushed Error" about 1 s later, fuzzy's other peer's 2 rails and misty's 4 rails stopped. Rebinding (`linkloss-after-rebind-noreload`) re-created the peer at once but WRs still failed (ENOTCONN) | fail as designed |
| Link loss with armed credit, receiver side (`linkloss-armed-unbind-receiver`) | Disconnect aborts the armed credit | `credit_aborted+1`, holder process passed, fuzzy's other peer's 2 rails stopped | fail as designed |
| Link loss with armed credit, sender side (`linkloss-armed-unbind-sender`) | Receiver keeps the credit; QP destroy cancels it | The verifier's sender had already exited. After unbinding misty's services and rebinding them, the holder's QP destroy cancelled and released (`canceled+1`, `returned+1`), no quarantine | pass (weak: no transfer was running) |
| Physical cable pull, unload of `thunderbolt` | not done | The 2026-10-08 report needed an ACPI NHI reset after link loss and I may not reset NHIs. Service unbind only | skipped |
| Raw GPU frame loss/corruption, parallel QPs, GPU reset | not covered | | skipped |

Gufo probe (`gufo-*`, every sum verified unless stated): baseline 1-row p50
30 us, p99 79/71 us. Lost DONE x20 per host: p99 about 11 ms (10 ms STATUS wait).
Lost CREDIT x20: no change. COMMIT delayed 50 ms x10 (after the fix): p99 about
51 ms. Lost COMMIT: the probe reported "verbs communicator is poisoned"
(completion status 12, retry exceeded) on rank 0 and the closed bootstrap on rank 1.

## Bugs and changes

- **Fixed:** a COMMIT arriving more than the 10 ms DONE wait late made the sender
  abort, stop its rails and flush both peers. Commit 14d53e1: STATUS now answers
  BUSY while the receiver still holds the matching speculative credit. A truly
  lost COMMIT still ends in quarantine, now after the sender's WR deadline
  (about 8.6 s with the verifier's QP timeout, 1.07 s x 8) instead of at once.
  The XDomain notification and the NHI frames travel on different channels, so
  frames overtaking a slow COMMIT was a real race, not only an injected one.
- **Fixed:** quarantine was silent. A ratelimited kernel message and a
  `gpu_stopped` field now show it.
- **Found in my own change, fixed before commit:** the first version flushed the
  system workqueue on unload ("Flushing system-wide workqueues will be
  prohibited" in dmesg). Delayed sends now use a private workqueue.
- **Fixed in the test tooling:** the verifier's new hold option. A first version
  of my kill script matched its own shell (`pkill -f`); `-x` is used.

## Recovery

Every quarantine was cleared by reloading both modules (about 12 s with the lab
script). After the link-loss tests, a reload of both modules restored normal
operation: 1 MiB 422/417 us per message versus 405/411 us before, 10,304 B
116/117 versus 114/115 us. The run-to-run difference is within noise seen
elsewhere. Data interrupts counted up again, and no NHI reset was used. The full
matrix with `--remote-error` passed after all fault cases and a reload
(`matrix-full-post-faults`). Reconnect after a peer module reload took 3 s
(reloading fuzzy) to 17 s (reloading misty; HELLO failed five times first,
then succeeded). Taint stayed 4096 and no WARN/BUG appeared except the
workqueue warning above, before its fix.

## Open problems

1. After a quarantine, a WR on the other rank can wait 30 s or more with no
   error CQE (lost RELEASE, CREDIT+CANCEL lost, one kill trial). The verifier
   stopped waiting at 30 s; the real bound was not measured.
2. A duplicate or very late COMMIT or RELEASE stops the rails. XDomain does not
   duplicate messages, so this is policy rather than a defect, but a RELEASE
   delayed beyond 500 ms is indistinguishable from a lost one.
3. One killed process during a transfer, or any partial link loss, takes the
   whole connection (both cables) down for all users until both modules are
   reloaded. Rebinding services is not enough.
4. A dead peer is not reported to the surviving QP: it either gets a flush after
   about 1 s or waits indefinitely. There is no keepalive.
5. ABORT for an armed credit is answered -ENOENT and leaves it armed until QP
   destroy; not changed here.
6. The kill-between-COMMIT-and-frames and physical-unplug cases need another
   method (a fault that holds frames back, or a controlled cable pull and an
   agreed NHI reset procedure).

## Evidence

[Raw logs, debugfs before/after, kernel log excerpts, fault hit counts and
the scripts](https://github.com/neuhaus/donnerkeule-results/tree/main/2026-10-18-verbs-loss/). Large traces were not kept. The first
sender-side link-loss attempt and a setup failure after an accidental module
restore are not included.
