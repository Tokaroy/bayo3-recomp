#!/usr/bin/env python3
"""Compile and run the debug-command parsing test."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
compiler = os.environ.get("CXX")
command = shlex.split(compiler) if compiler else [shutil.which("clang++") or shutil.which("g++") or "c++"]

with tempfile.TemporaryDirectory(prefix="b3-debug-") as temp:
    exe = Path(temp) / ("b3_debug_test.exe" if os.name == "nt" else "b3_debug_test")
    subprocess.run(
        command + [
            "-std=c++20", "-O1", "-pthread", "-I", str(ROOT / "runtime/include"),
            str(ROOT / "tests/b3_debug_test.cpp"), "-o", str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
    print("PASS b3_debug", flush=True)
