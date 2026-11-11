#!/usr/bin/env python3
"""Run hardware-independent protocol and production state smoke tests."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
C_TESTS = {
    "verbs-dmabuf": [],
    "proto": [],
    "reliability": ["proto/reliability.c"],
    "identity": ["proto/identity.c"],
    "config": ["proto/config.c", "proto/identity.c"],
}
with tempfile.TemporaryDirectory(prefix="tbv-smoke-") as directory:
    for name, sources in C_TESTS.items():
        binary = str(Path(directory) / name)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11",
            "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
            "-I", str(ROOT), str(ROOT / f"tools/ci/{name}-smoke.c"),
            *(str(ROOT / source) for source in sources), "-o", binary,
        ], check=True)
        subprocess.run([binary], check=True)
for name in ("source-layout", "provider-patches", "verbs-control-state", "verbs-control-lifetime",
             "verbs-dma-wait", "verbs-post", "verbs-rx-state", "host-credit-peek", "configfs-state", "bench-helpers", "profiling", "framework-desktop", "rx-error-recovery"):
    subprocess.run([sys.executable, str(ROOT / f"tools/ci/{name}-smoke.py")], check=True)
