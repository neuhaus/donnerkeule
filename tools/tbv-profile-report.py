#!/usr/bin/env python3
"""Summarize capture directories and export a Perfetto/Chrome JSON timeline.

Uses payload CLOCK_MONOTONIC timestamps, never the rounded ftrace prefix.
Hosts/runs have separate origins. GPU event durations never pretend to share
the CPU clock. Nested/parallel spans are not summed into a transfer latency.
"""
import argparse
from collections import defaultdict, deque
from decimal import Decimal
import json
import math
from pathlib import Path
import re
import statistics

KERNEL_FIELDS = {"phase", "token", "wr_id", "src_qp", "dst_qp", "psn", "offset",
                 "bytes", "rail", "tx", "opcode", "start_ns", "end_ns", "status", "aux"}
TX_LEDGER = {"tx_queue", "tx_paths_lock", "batch_build", "control_exchange",
             "prepare_retry_sleep", "batch_post", "tx_dma_wait", "done_wait",
             "status_retry_sleep", "batch_release", "tx_finish"}


def percentile(values, percent):
    ordered = sorted(values)
    where = (len(ordered) - 1) * percent / 100
    lo, hi = math.floor(where), math.ceil(where)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (where - lo)


def distribution(values):
    return {"count": len(values), "sum": sum(values), "mean": statistics.mean(values),
            "min": min(values), "p50": percentile(values, 50), "p95": percentile(values, 95),
            "p99": percentile(values, 99), "max": max(values)}


def kernel_records(text, problems):
    records = []
    for line_number, line in enumerate(text.splitlines(), 1):
        if "LOST" in line or "LOST EVENTS" in line:
            problems.append(f"kernel line {line_number}: lost-event marker")
        if "tbv_perf:" not in line:
            continue
        try:
            payload = line.split("tbv_perf:", 1)[1]
            fields = dict(re.findall(r"(\w+)=([^\s]+)", payload))
            if KERNEL_FIELDS - fields.keys():
                raise ValueError(f"missing {sorted(KERNEL_FIELDS - fields.keys())}")
            row = {k: v if k == "phase" else int(v) for k, v in fields.items() if k in KERNEL_FIELDS}
            if row["start_ns"] <= 0 or row["end_ns"] < row["start_ns"]:
                raise ValueError("invalid monotonic interval")
            row.update(kind="kernel", duration_ns=row["end_ns"] - row["start_ns"])
            records.append(row)
        except (ValueError, TypeError) as error:
            problems.append(f"kernel line {line_number}: {error}")
    return records


def gpu_records(path, problems, metadata_out=None):
    records, metadata, footer, configuration = [], None, None, None
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        try:
            row = json.loads(line)
            if row["kind"] == "metadata":
                if metadata is not None or row.get("schema") != 1 or row.get("clock") != "monotonic":
                    raise ValueError("duplicate or incompatible metadata")
                metadata = row
            elif row["kind"] == "configuration":
                if configuration is not None or metadata is None or records or footer is not None:
                    raise ValueError("duplicate or misplaced configuration")
                configuration = row
            elif row["kind"] == "footer":
                if footer is not None:
                    raise ValueError("duplicate footer")
                footer = row
            elif row["kind"] in ("cpu", "gpu", "counter"):
                if footer is not None:
                    raise ValueError("record after footer")
                if metadata is None:
                    raise ValueError("record before metadata")
                for key in ("phase", "bytes", "qpn", "op", "start_ns", "end_ns",
                            "duration_ns", "status", "aux", "wr_id", "round", "iteration", "generation"):
                    if key not in row:
                        raise ValueError(f"missing {key}")
                if row["duration_ns"] < 0 or row["aux"] < 0:
                    raise ValueError("negative duration/counter")
                if row["kind"] == "cpu" and (row["start_ns"] <= 0 or
                    row["end_ns"] - row["start_ns"] != row["duration_ns"]):
                    raise ValueError("invalid CPU interval")
                if row["kind"] != "cpu" and (row["start_ns"] or row["end_ns"]):
                    raise ValueError("GPU/counter assigned CPU timestamps")
                records.append(row)
            else:
                raise ValueError("unknown record kind")
        except (ValueError, TypeError, KeyError) as error:
            problems.append(f"{path.name} line {line_number}: {error}")
    if metadata is None or footer is None:
        problems.append(f"{path.name}: missing metadata/footer")
    elif not footer.get("complete") or footer.get("dropped") or footer.get("records") != len(records):
        problems.append(f"{path.name}: incomplete/dropped/count mismatch: {footer}")
    if metadata_out is not None:
        metadata_out.update(metadata=metadata, configuration=configuration, footer=footer)
    return records


def covered_ns(intervals, start, end):
    clipped = sorted((max(start, a), min(end, b)) for a, b in intervals if b > start and a < end)
    total, cursor = 0, start
    for a, b in clipped:
        if b > max(cursor, a):
            total += b - max(cursor, a)
        cursor = max(cursor, b)
    return total


def derive(records, problems):
    groups, output = defaultdict(list), []
    for row in records:
        if row["kind"] == "kernel" and row["token"]:
            session = row["wr_id"] if row["phase"].startswith("gpu_") else 0
            groups[(row["token"], row["src_qp"], row["dst_qp"], row["psn"], row["tx"], session)].append(row)
    for key, rows in groups.items():
        if key[4] and any(r["phase"] in ("tx_wr", "tx_job", "batch", "batch_build", "lane_post") for r in rows):
            for phase in ("tx_wr", "tx_queue", "tx_job", "tx_finish"):
                count = sum(r["phase"] == phase for r in rows)
                if count != 1:
                    problems.append(f"partial/ambiguous TX WR {key}: {phase} count={count}")
        for wr in (r for r in rows if r["phase"] == "tx_wr"):
            children = [r for r in rows if r["phase"] in TX_LEDGER and not r["rail"]]
            covered = covered_ns([(r["start_ns"], r["end_ns"]) for r in children], wr["start_ns"], wr["end_ns"])
            output.append(dict(wr, kind="derived", phase="tx_unattributed", start_ns=0, end_ns=0,
                               duration_ns=wr["duration_ns"] - covered))
        batches = defaultdict(list)
        for row in rows:
            batches[(row["offset"], row["rail"])].append(row)
        for batch, events in batches.items():
            phases = defaultdict(list)
            for row in events:
                phases[row["phase"]].append(row)
            for first_phase, last_phase, name, first_edge, last_edge in (
                ("dma_first", "dma_last", "dma_callback_span", "start_ns", "end_ns"),
                ("lane_post", "dma_first", "post_to_first_callback", "start_ns", "end_ns"),
                ("batch_dma_done", "tx_dma_wait", "tx_callback_resume", "start_ns", "end_ns"),
                ("gpu_dma_first", "gpu_dma_last", "gpu_dma_callback_span", "start_ns", "end_ns"),
                ("gpu_lane_post", "gpu_dma_first", "gpu_post_to_first_callback", "start_ns", "end_ns"),
                ("gpu_batch_dma_done", "gpu_wait", "gpu_callback_resume", "start_ns", "end_ns"),
            ):
                if len(phases[first_phase]) == 1 and len(phases[last_phase]) == 1:
                    first, last = phases[first_phase][0], phases[last_phase][0]
                    start, end = first[first_edge], last[last_edge]
                    if end >= start:
                        output.append(dict(last, phase=name, kind="derived", start_ns=start,
                                           end_ns=end, duration_ns=end - start))
                elif phases[first_phase] and phases[last_phase]:
                    problems.append(f"ambiguous {name}: WR {key}, batch/rail {batch}")
    cqs = defaultdict(deque)
    for row in sorted(records, key=lambda r: r["end_ns"]):
        if row["kind"] == "kernel" and row["phase"] == "cq_publish":
            cqs[(row["src_qp"], row["wr_id"], row["opcode"])].append(row)
        elif row["kind"] == "cpu" and row["phase"] == "cq_seen":
            candidates = cqs[(row["qpn"], row["wr_id"], row["aux"])]
            if candidates:
                placed = candidates.popleft()
                output.append(dict(row, kind="derived_cpu", phase="cq_visibility_delay",
                                   start_ns=placed["end_ns"],
                                   duration_ns=row["end_ns"] - placed["end_ns"]))
    return output


def system_records(text):
    """Retain opt-in scheduler/IRQ/workqueue marks at ftrace prefix precision."""
    rows = []
    pattern = re.compile(r"\[(\d+)\].*?\s(\d+\.\d+):\s+(\w+):\s*(.*)")
    for line in text.splitlines():
        match = pattern.search(line)
        if not match:
            continue
        cpu, timestamp, phase, payload = match.groups()
        if phase.startswith(("sched_", "irq_", "softirq_", "workqueue_")):
            ns = int(Decimal(timestamp) * 1_000_000_000)
            rows.append({"kind": "system", "phase": phase, "cpu": int(cpu), "payload": payload,
                         "start_ns": ns, "end_ns": ns, "duration_ns": 0,
                         "timestamp_precision": "ftrace_prefix"})
    return rows


def load_capture(label, directory):
    problems = []
    metadata = json.loads((directory / "capture.json").read_text())
    if metadata.get("schema") != 1 or metadata.get("clock") != "monotonic":
        problems.append("incompatible capture schema/clock")
    if not metadata.get("complete"):
        problems.append("capture command failed, interrupted or timed out")
    before, after = metadata.get("before", {}), metadata.get("after", {})
    for field in ("module_parameters", "module_build_id_note"):
        if before.get(field) != after.get(field):
            problems.append(f"{field} changed during capture")
    if not metadata.get("per_cpu_stats"):
        problems.append("missing trace buffer loss statistics")
    for cpu, stats in metadata.get("per_cpu_stats", {}).items():
        for name, value in re.findall(r"^\s*(overrun|commit overrun|dropped events):\s*(\d+)", stats, re.MULTILINE):
            if int(value):
                problems.append(f"{cpu}: {name}={value}")
    raw = (directory / "kernel.trace").read_text()
    records = kernel_records(raw, problems)
    if not records:
        problems.append("no tbv_perf records")
    gpu = directory / "gpu.jsonl"
    gpu_metadata = {}
    if gpu.exists():
        records.extend(gpu_records(gpu, problems, gpu_metadata))
    records.extend(derive(records, problems))
    records.extend(system_records(raw))
    failures = sum(r.get("status", 0) < 0 for r in records if r["kind"] != "derived")
    recoveries = {phase: sum(r["phase"] == phase for r in records)
                  for phase in ("dma_watchdog", "gpu_dma_watchdog", "status_fallback", "prepare_retry_sleep", "status_retry_sleep")}
    if failures:
        problems.append(f"{failures} records with negative error status (retained)")
    for phase, count in recoveries.items():
        if count:
            problems.append(f"{phase}: {count} events (retained)")
    return {"label": label, "path": str(directory), "metadata": metadata,
            "gpu_metadata": gpu_metadata, "problems": problems,
            "recoveries": recoveries, "records": records}


def summarize(captures):
    output = []
    for capture in captures:
        groups = defaultdict(list)
        for row in capture["records"]:
            if row["kind"] == "system":
                continue
            direction = "tx" if row.get("tx") else "rx" if "tx" in row else "app"
            groups[(row["kind"], row["phase"], row["bytes"], direction,
                    str(row.get("op", row.get("opcode", ""))))].append(row)
        for (kind, phase, size, direction, op), rows in sorted(groups.items()):
            values = [r["aux"] if kind == "counter" else r["duration_ns"] for r in rows]
            output.append({"capture": capture["label"], "kind": kind, "phase": phase,
                           "bytes": size, "direction": direction, "op": op,
                           "unit": "value" if kind == "counter" else "ns",
                           "errors": sum(r.get("status", 0) < 0 for r in rows),
                           **distribution(values)})
    return output


def phase_totals(captures):
    output = []
    for capture in captures:
        groups = defaultdict(list)
        for row in capture["records"]:
            if row["kind"] in ("system", "counter"):
                continue
            direction = "tx" if row.get("tx") else "rx" if "tx" in row else "app"
            groups[(row["kind"], row["phase"], direction)].append(row["duration_ns"])
        for (kind, phase, direction), values in sorted(groups.items()):
            output.append({"capture": capture["label"], "kind": kind, "phase": phase,
                           "direction": direction, "calls": len(values),
                           "total_ns": sum(values), "mean_ns": statistics.mean(values),
                           "max_ns": max(values)})
    return output


def timeline(captures):
    events = []
    for pid, capture in enumerate(captures, 1):
        events.append({"ph": "M", "name": "process_name", "pid": pid,
                       "args": {"name": capture["label"] + " (local monotonic clock)"}})
        spans = [r for r in capture["records"] if r.get("start_ns", 0)]
        origin = min((r["start_ns"] for r in spans), default=0)
        tracks = {}
        for row in spans:
            track = (row["kind"], row.get("tx", "app"), row.get("src_qp", row.get("qpn")),
                     row.get("dst_qp", 0), row.get("token", 0), row.get("rail", row.get("cpu", 0)), row["phase"])
            if track not in tracks:
                tracks[track] = len(tracks) + 1
                events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": tracks[track],
                               "args": {"name": "/".join(map(str, track))}})
            args = {k: str(v) if k in ("token", "wr_id") else v for k, v in row.items()
                    if k not in ("start_ns", "end_ns", "duration_ns")}
            point = row["start_ns"] == row["end_ns"]
            event = {"ph": "i" if point else "X", "name": row["phase"], "cat": row["kind"],
                     "pid": pid, "tid": tracks[track], "ts": (row["start_ns"] - origin) / 1000,
                     "args": args}
            event.update({"s": "t"} if point else {"dur": row["duration_ns"] / 1000})
            events.append(event)
    return {"traceEvents": events, "displayTimeUnit": "ms"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="append", required=True, help="LABEL=CAPTURE_DIR")
    parser.add_argument("--out", type=Path, required=True, help="new report directory")
    parser.add_argument("--require-clean", action="store_true")
    args = parser.parse_args()
    captures = []
    for spec in args.capture:
        label, sep, directory = spec.partition("=")
        if not sep or not label or any(c["label"] == label for c in captures):
            parser.error("each --capture needs a unique LABEL=DIR")
        captures.append(load_capture(label, Path(directory)))
    stats = summarize(captures)
    totals = phase_totals(captures)
    args.out.mkdir(parents=True, exist_ok=False)
    receipt = [{k: v for k, v in c.items() if k != "records"} for c in captures]
    (args.out / "summary.json").write_text(json.dumps({"captures": receipt, "stats": stats, "phase_totals": totals}, indent=2) + "\n")
    (args.out / "timeline.json").write_text(json.dumps(timeline(captures)) + "\n")
    lines = ["# Transport profile", "", "Diagnostic timings include profiling overhead. GPU durations use a separate clock.",
             "Nested and parallel rows are not additive. No cross-host clock subtraction.", ""]
    for c in captures:
        lines += [f"## {c['label']}", "", f"Records: {len(c['records'])}.", ""]
        lines += [f"- {p}" for p in c["problems"]] or ["No capture/loss/recovery problems detected."]
        lines += [""]
    lines += ["## Phase totals", "", "Aggregate calls and durations across all message sizes (times in µs).", "",
              "| Capture | Kind | Phase | Dir | Calls | Total | Mean | Max |", "|---|---|---|---|---:|---:|---:|---:|"]
    for row in totals:
        lines.append(f"| {row['capture']} | {row['kind']} | {row['phase']} | {row['direction']} | {row['calls']} | " +
                     " | ".join(f"{row[key] / 1000:.3f}" for key in ("total_ns", "mean_ns", "max_ns")) + " |")
    lines += ["", "## Phase distributions", "", "Times in µs; counters in their stated units. All samples, including failures, retained.", "",
              "| Capture | Kind | Phase | Bytes | Dir | Op | N | P50 | P95 | P99 | Max |", "|---|---|---|---:|---|---|---:|---:|---:|---:|---:|"]
    for row in stats:
        scale = 1000 if row["unit"] == "ns" else 1
        lines.append(f"| {row['capture']} | {row['kind']} | {row['phase']} | {row['bytes']} | {row['direction']} | {row['op']} | {row['count']} | " +
                     " | ".join(f"{row[key] / scale:.3f}" for key in ("p50", "p95", "p99", "max")) + " |")
    (args.out / "report.md").write_text("\n".join(lines) + "\n")
    print(f"Profile report saved: {args.out}")
    return 2 if args.require_clean and any(c["problems"] for c in captures) else 0


if __name__ == "__main__":
    raise SystemExit(main())
