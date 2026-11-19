#!/usr/bin/env python3
"""Exercise production profiling helpers, bounded recording and capture cleanup.

Mock HIP/tracefs tests do not qualify Linux ABI, interrupts or GPU clocks.
"""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

from smoke_support import ROOT, functions, run_c


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


report = load("tbv_profile_report", ROOT / "tools/tbv-profile-report.py")
capture = load("tbv_profile_capture", ROOT / "tools/tbv-profile-capture.py")


with tempfile.TemporaryDirectory(prefix="tbv-profile-smoke-") as directory:
    temp = Path(directory)
    # Include the actual kernel helper header, with only Linux's trace machinery
    # and time source stubbed. Check both static-key OFF and compiled-out builds.
    (temp / "linux").mkdir()
    (temp / "trace").mkdir()
    (temp / "linux/types.h").write_text("#include <stdint.h>\n#include <stdbool.h>\ntypedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;\n")
    (temp / "linux/tracepoint.h").write_text("#define TRACE_EVENT(...)\n")
    (temp / "linux/timekeeping.h").write_text("u64 fake_clock(void);\n#define ktime_get_ns fake_clock\n")
    (temp / "trace/define_trace.h").write_text("")
    source = temp / "helper.c"
    source.write_text(r'''
#include <assert.h>
#include <errno.h>
#include "kernel/perf_types.h"
static bool enabled;
static unsigned clocks, records;
static struct tbv_perf_id saved;
static u64 first, last;
static int result;
u64 fake_clock(void) { clocks++; return clocks * 100; }
static inline __attribute__((unused)) bool trace_tbv_perf_enabled(void) { return enabled; }
static inline __attribute__((unused)) void trace_tbv_perf(const struct tbv_perf_id *id, const char *phase,
                                  u64 a, u64 b, int status, u32 aux) {
    (void)phase; (void)aux; records++; saved = *id; first = a; last = b; result = status;
}
#include "kernel/perf.h"
int main(void) {
    struct tbv_dmabuf_wire m = {.token = 0xfedcba9876543210ull, .offset = 8192,
        .bytes = 32, .hdr = {.src_qp = 7, .dest_qp = 9, .psn = 11, .opcode = 2}};
    assert(tbv_perf_clock() == 0);
    tbv_perf_wire(&m, true, "off", 0, 0, 0);
    tbv_perf_wire_mark(&m, true, "off", 0, 0);
    assert(!clocks && !records);
    enabled = true;
#ifdef TBV_DISABLE_PROFILING
    tbv_perf_wire_mark(&m, true, "compiled-out", 0, 0);
    assert(tbv_perf_clock() == 0 && !clocks && !records);
    (void)saved; (void)first; (void)last; (void)result;
#else
    u64 start = tbv_perf_clock();
    tbv_perf_wire(&m, true, "span", start, -ETIMEDOUT, 5);
    assert(clocks == 2 && records == 1 && first == 100 && last == 200 && result == -ETIMEDOUT);
    assert(saved.token == m.token && saved.src_qp == 7 && saved.dst_qp == 9 && saved.psn == 11);
    assert(saved.offset == 8192 && saved.bytes == 32 && saved.tx);
    tbv_perf_wire_mark(&m, false, "mark", 2, 0);
    assert(records == 2 && first == last && !saved.tx);
    tbv_perf_interval(&saved, "invalid", 20, 10, 0, 0);
    tbv_perf_interval(&saved, "invalid", 0, 10, 0, 0);
    assert(records == 2);
    enabled = false;
    tbv_perf_span(&saved, "disabled-during-span", start, 0, 0);
    assert(records == 2 && clocks == 3);
#endif
    return 0;
}
''')
    for disabled in (False, True):
        binary = temp / f"helper-{disabled}"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-parameter", "-g", "-fsanitize=address,undefined",
                        "-I", str(temp), "-I", str(ROOT), *( ["-DTBV_DISABLE_PROFILING"] if disabled else []),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    # Include the actual bounded JSONL recorder and GPU marker owner. No HIP SDK
    # required to verify OFF does no API work or ON reuses/releases all events.
    gpu_source = (ROOT / "userspace/bench/tbv_gpu_verbs.hip").read_text()
    timing = gpu_source[gpu_source.index("struct GpuTiming {"):gpu_source.index("static uint64_t process_cpu_us()")]
    source = temp / "recorder.cpp"
    source.write_text(r'''
#include <cassert>
#include <array>
#include "userspace/bench/tbv_profile.h"
struct Event {}; using hipEvent_t = Event *; using hipStream_t = void *;
static unsigned creates, destroys, event_records, elapsed;
inline int hipEventCreate(hipEvent_t *p) { *p = new Event; creates++; return 0; }
inline int hipEventDestroy(hipEvent_t p) { delete p; destroys++; return 0; }
inline int hipEventRecord(hipEvent_t, hipStream_t) { event_records++; return 0; }
inline int hipEventElapsedTime(float *ms, hipEvent_t, hipEvent_t) { *ms = 0.125f; elapsed++; return 0; }
#define HIP(call) assert((call) == 0)
''' + timing + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    TbvProfile off("");
    { GpuTiming timing(off.enabled()); timing.begin(0, "producer", 0, nullptr);
      timing.end(0, nullptr); timing.collect(off); }
    off.cpu("off", 1, 2); off.counter("off", 1);
    assert(!off.now() && !creates && !event_records && !elapsed && !destroys);
#ifdef TBV_DISABLE_PROFILING
    bool rejected = false;
    try { TbvProfile on(argv[1]); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
#else
    {
        TbvProfile on(argv[1], 3);
        on.configuration(1, false, 1, 50, true, false, 128, 1);
        on.context = {1, 2, 3, 4, 10304, 1, 7, "WRITE_IMM"};
        on.cpu("cycle", 100, 120, 0xfedcba9876543210ull);
        { GpuTiming timing(on.enabled());
          timing.begin(0, "producer", 0, nullptr); timing.end(0, nullptr); timing.collect(on);
          timing.collect(on); // Already consumed: no duplicate measurements.
        }
        on.flush();
        on.counter("empty_polls", 19); on.counter("dropped-1", 1); on.flush();
        on.counter("dropped-2", 2); on.complete();
    }
    assert(creates == 28 && destroys == 28 && event_records == 2 && elapsed == 1);
    bool rejected = false;
    try { TbvProfile overwrite(argv[1]); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
#endif
    return 0;
}
''')
    for disabled in (False, True):
        binary, output = temp / f"recorder-{disabled}", temp / f"gpu-{disabled}.jsonl"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra",
                        "-Werror", "-g", "-fsanitize=address,undefined", "-I", str(ROOT),
                        *( ["-DTBV_DISABLE_PROFILING"] if disabled else []),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(output)], check=True)
        if disabled:
            assert not output.exists()
        else:
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            assert rows[-1] == {"kind": "footer", "complete": True, "records": 3, "dropped": 2}
            assert rows[1]["kind"] == "configuration" and rows[1]["spin_us"] == 50
            assert rows[2]["wr_id"] == 0xfedcba9876543210
            assert rows[3]["kind"] == "gpu" and rows[3]["duration_ns"] == 125000
            problems = []
            detail = {}
            assert len(report.gpu_records(output, problems, detail)) == 3 and problems
            assert detail["configuration"]["persistent_rq"] is True

    # Synthetic critical path with nested durations, >53-bit timestamps/token,
    # parallel rail callbacks and independent host clock origins.
    base = 1_000_000_000_000_000_000
    def record(phase, start, end, rail=0, status=0, **extra):
        fields = dict(phase=phase, token=0xfedcba9876543210, wr_id=0, src_qp=7, dst_qp=9, psn=11,
                      offset=0, bytes=10304, rail=rail, tx=1, opcode=4,
                      start_ns=base + start, end_ns=base + end, status=status, aux=0)
        fields.update(extra)
        return "task-123 [000] .... 1.000000: tbv_perf: " + " ".join(f"{k}={v}" for k, v in fields.items())
    raw = "\n".join([record("tx_wr", 0, 1000), record("tx_queue", 0, 100), record("tx_job", 100, 950),
        record("batch_build", 100, 200), record("lane_alloc", 120, 170, 1),
        record("control_exchange", 200, 400), record("xdomain_request", 210, 399),
        record("batch_post", 400, 500), record("tx_dma_wait", 500, 800),
        record("batch_release", 800, 900), record("tx_finish", 900, 950),
        record("lane_post", 400, 450, 1), record("dma_first", 460, 460, 1),
        record("dma_last", 780, 780, 1), record("batch_dma_done", 790, 790)])
    problems = []
    rows = report.kernel_records(raw, problems)
    derived = report.derive(rows, problems)
    assert not problems
    durations = {r["phase"]: r["duration_ns"] for r in derived}
    assert durations == {"tx_unattributed": 50, "dma_callback_span": 320,
                         "post_to_first_callback": 60, "tx_callback_resume": 10}
    exclusive = "\n".join(record(phase, start + shift, end + shift, wr_id=session)
        for session, shift in ((3, 0), (4, 2000))
        for phase, start, end in (("gpu_lane_post", 0, 50), ("gpu_dma_first", 60, 60),
            ("gpu_dma_last", 780, 780), ("gpu_batch_dma_done", 790, 790), ("gpu_wait", 50, 800)))
    problems = []
    gpu_derived = report.derive(report.kernel_records(exclusive, problems), problems)
    assert not problems and len(gpu_derived) == 6
    assert {r["wr_id"] for r in gpu_derived} == {3, 4}
    assert {r["phase"]: r["duration_ns"] for r in gpu_derived} == {
        "gpu_post_to_first_callback": 60, "gpu_dma_callback_span": 720, "gpu_callback_resume": 10}
    cq = report.kernel_records(record("cq_publish", 100, 100, token=0, wr_id=23, opcode=1), [])
    seen = {"kind": "cpu", "phase": "cq_seen", "qpn": 7, "wr_id": 23, "aux": 1,
            "start_ns": base + 140, "end_ns": base + 140, "duration_ns": 0,
            "bytes": 10304, "op": "WRITE_IMM", "status": 0}
    visibility = report.derive(cq + [seen], [])
    assert visibility[0]["phase"] == "cq_visibility_delay" and visibility[0]["duration_ns"] == 40
    assert not report.derive(cq + [dict(seen, wr_id=99)], [])
    system = report.system_records("worker-3 [007] .... 123.000456: irq_handler_entry: irq=31 name=nhi\n")
    assert system[0]["start_ns"] == 123000456000 and system[0]["cpu"] == 7
    assert report.covered_ns([(0, 20), (10, 30), (40, 80)], 5, 50) == 35
    assert report.distribution([10, 20])["p95"] == 19.5
    cap = temp / "clean"; cap.mkdir()
    meta = {"schema": 1, "clock": "monotonic", "complete": True,
            "per_cpu_stats": {"cpu0": "overrun: 0\ncommit overrun: 0\ndropped events: 0\n"}}
    (cap / "capture.json").write_text(json.dumps(meta))
    (cap / "kernel.trace").write_text(raw)
    loaded = report.load_capture("fuzzy", cap)
    assert not loaded["problems"]
    other = dict(loaded, label="misty", records=[dict(r, start_ns=r["start_ns"] + 10**12,
                     end_ns=r["end_ns"] + 10**12) for r in rows])
    events = report.timeline([loaded, other])["traceEvents"]
    assert all(e["ts"] >= 0 for e in events if "ts" in e)
    assert [e["ts"] for e in events if e["name"] == "tx_wr"] == [0, 0]
    assert next(e["args"]["token"] for e in events if e["name"] == "tx_wr") == str(0xfedcba9876543210)
    subprocess.run([sys.executable, str(ROOT / "tools/tbv-profile-report.py"), "--capture", f"test={cap}",
                    "--out", str(temp / "report-clean"), "--require-clean"], check=True)
    clean_summary = json.loads((temp / "report-clean/summary.json").read_text())
    tx_wr_total = next(t for t in clean_summary["phase_totals"] if t["phase"] == "tx_wr" and t["direction"] == "tx")
    assert tx_wr_total["calls"] == 1 and tx_wr_total["total_ns"] == 1000 and tx_wr_total["max_ns"] == 1000
    assert "## Phase totals" in (temp / "report-clean/report.md").read_text()
    meta["per_cpu_stats"]["cpu0"] = "overrun: 3\ncommit overrun: 0\ndropped events: 0\n"
    (cap / "capture.json").write_text(json.dumps(meta))
    (cap / "kernel.trace").write_text(raw + "\n" + record("dma_watchdog", 999, 999) + "\n" + "tbv_perf: phase=broken")
    result = subprocess.run([sys.executable, str(ROOT / "tools/tbv-profile-report.py"), "--capture", f"test={cap}",
                             "--out", str(temp / "report-loss"), "--require-clean"])
    assert result.returncode == 2
    assert len(json.loads((temp / "report-loss/summary.json").read_text())["captures"][0]["problems"]) == 3

    # Fake only tracefs creation/removal. Execute real successful/failing/timed
    # commands and verify event disabling/instance cleanup on every path.
    tracefs = temp / "tracefs"; (tracefs / "instances").mkdir(parents=True)
    (tracefs / "tracing_on").write_text("global-tracer-untouched")
    def make_instance(path):
        path.mkdir()
        for name in ("tracing_on", "buffer_size_kb", "trace_clock", "trace"):
            (path / name).write_text("")
        event = path / "events/thunderbolt_ibverbs/tbv_perf"; event.mkdir(parents=True)
        (event / "enable").write_text("0")
        cpu = path / "per_cpu/cpu0"; cpu.mkdir(parents=True)
        (cpu / "stats").write_text("overrun: 0\ncommit overrun: 0\ndropped events: 0\n")
    def remove_instance(path):
        assert (path / "tracing_on").read_text() == "0"
        assert (path / "events/thunderbolt_ibverbs/tbv_perf/enable").read_text() == "0"
        shutil.rmtree(path)
    for name, command, expected in (("success", [sys.executable, "-c", "print('ok')"], 0),
                                     ("failure", [sys.executable, "-c", "raise SystemExit(7)"], 1),
                                     ("signal", [sys.executable, "-c", "import os,signal; os.kill(os.getppid(),signal.SIGTERM)"], 1),
                                     ("timeout", [sys.executable, "-c", "import time; time.sleep(10)"], 1)):
        out = temp / f"capture-{name}"
        with patch.object(capture, "create_instance", make_instance), patch.object(capture, "remove_instance", remove_instance), \
             patch.object(capture, "snapshot", lambda: {"monotonic_ns": 1}), \
             patch.object(sys, "argv", ["capture", "--tracefs", str(tracefs), "--out", str(out), "--timeout", "1", "--", *command]):
            assert capture.main() == expected
        assert not list((tracefs / "instances").iterdir())
        saved = json.loads((out / "capture.json").read_text())
        assert saved["complete"] == (expected == 0)
        assert saved["timed_out"] == (name == "timeout")
        assert (tracefs / "tracing_on").read_text() == "global-tracer-untouched"

# Exercise the actual per-frame callback: profiling keeps DMA/error/wakeup
# semantics and emits only first/last marks per rail, never one trace per frame.
callback = functions((ROOT / "kernel/verbs_dmabuf.c").read_text(), ("tbv_dmabuf_complete",))
run_c("profile-callback", r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int atomic_t;
struct tbv_perf_id { unsigned rail; };
struct tbv_state { u64 data_tx_canceled, data_rx_canceled, data_tx_completed,
                         data_rx_completed, data_rx_bad_frame; };
struct tbv_peer { struct tbv_state *state; };
struct tbv_rail;
struct tbv_path { struct tbv_rail *rail; u64 data_tx_completed, data_rx_completed, tx_bytes, rx_bytes; };
struct tbv_rail { struct tbv_peer *peer; struct tbv_path path; };
struct tbv_dmabuf_lane { struct tbv_rail *rail; bool perf_active; atomic_t perf_pending;
                         u32 count; struct tbv_perf_id perf; };
struct tbv_dmabuf_batch { atomic_t error, pending; int wait; struct tbv_perf_id perf; u64 perf_dma_done_ns; };
struct ring_frame { u32 size, sof, eof, flags; };
struct tbv_dmabuf_frame { struct ring_frame frame; struct tbv_dmabuf_batch *batch;
                          struct tbv_dmabuf_lane *lane; u32 bytes; uint8_t eof; };
struct tb_ring { bool is_tx; };
#define TBV_DMABUF_FRAME_BYTES 4096
#define TBV_DATA_PDF_FRAME_START 1
#define RING_DESC_CRC_ERROR 1
#define RING_DESC_BUFFER_OVERRUN 2
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define atomic_read(p) (*(p))
#define atomic64_inc(p) (++*(p))
#define atomic64_add(value, p) (*(p) += (value))
#define atomic_dec_return(p) (--*(p))
#define atomic_dec_and_test(p) (!--*(p))
static inline int atomic_cmpxchg(int *p, int old, int next) { int value = *p; if (value == old) *p = next; return value; }
#define WRITE_ONCE(value, next) ((value) = (next))
#define pr_debug(...) ((void)0)
static unsigned wakes, queued, firsts, lasts, batch_marks, clocks;
static bool enabled, dmabuf_rx_eager = true;
static struct { int rx_complete_work; } dmabuf_engine;
#define system_highpri_wq NULL
#define wake_up_all(p) do { (void)(p); wakes++; } while (0)
#define queue_work(wq, work) do { (void)(wq); (void)(work); queued++; } while (0)
static bool tbv_control_enabled(void) { return false; }
static bool tbv_control_push_enabled(void) { return false; }
static u64 tbv_perf_clock(void) { if (!enabled) return 0; return ++clocks; }
static void tbv_perf_mark(const struct tbv_perf_id *id, const char *phase, int status, u32 aux) {
    (void)id; (void)status; (void)aux; if (!enabled) return;
    if (!strcmp(phase, "dma_first")) firsts++;
    else if (!strcmp(phase, "dma_last")) lasts++;
    else { assert(!strcmp(phase, "batch_dma_done")); batch_marks++; }
}
''' + callback + r'''
int main(void) {
    for (unsigned profiling = 0; profiling < 2; profiling++) {
        enabled = profiling; clocks = wakes = queued = firsts = lasts = batch_marks = 0;
        struct tbv_state state = {0}; struct tbv_peer peer = {.state = &state};
        struct tbv_rail rails[2] = {{.peer = &peer}, {.peer = &peer}};
        struct tbv_dmabuf_lane lanes[2] = {{.rail = &rails[0]}, {.rail = &rails[1]}};
        struct tbv_dmabuf_batch batch = {.pending = 4};
        struct tb_ring ring = {.is_tx = true};
        for (unsigned i = 0; i < 2; i++) {
            rails[i].path.rail = &rails[i]; lanes[i].count = 2;
            lanes[i].perf_pending = 2; lanes[i].perf_active = profiling;
            for (unsigned j = 0; j < 2; j++) {
                struct tbv_dmabuf_frame f = {.batch = &batch, .lane = &lanes[i], .bytes = 4096};
                tbv_dmabuf_complete(&ring, &f.frame, false);
            }
        }
        assert(!batch.pending && !batch.error && wakes == 1 && !queued);
        assert(state.data_tx_completed == 4 && rails[0].path.tx_bytes == 8192);
        assert(firsts == 2 * profiling && lasts == 2 * profiling && batch_marks == profiling);
        assert(!profiling ? (!clocks && lanes[0].perf_pending == 2) : (clocks == 1));
        ring.is_tx = false; batch.pending = 1; wakes = queued = 0;
        lanes[0].perf_active = false;
        struct tbv_dmabuf_frame bad = {.batch = &batch, .lane = &lanes[0], .bytes = 4096,
            .frame = {.sof = TBV_DATA_PDF_FRAME_START, .flags = RING_DESC_CRC_ERROR}};
        tbv_dmabuf_complete(&ring, &bad.frame, false);
        assert(batch.error == -EBADMSG && state.data_rx_bad_frame == 1 && wakes == 1 && queued == 1);
        batch.pending = 1; batch.error = 0;
        tbv_dmabuf_complete(&ring, &bad.frame, true);
        assert(batch.error == -ECANCELED && state.data_rx_canceled == 1);
    }
    puts("Profile callback: PASS (OFF/ON, two rails, completion, bad frame, cancellation)");
    return 0;
}
''')

print("Profiling: PASS (OFF/compiled-out clocks, bounded JSONL, GPU event lifetime, nested/parallel timing, precision, loss detection, cleanup)")
