#!/usr/bin/env python3
"""Compile and run the host movie decoder queue test."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
compiler = os.environ.get("CXX")
command = shlex.split(compiler) if compiler else [shutil.which("clang++") or shutil.which("g++") or "c++"]

with tempfile.TemporaryDirectory(prefix="language-locale-") as temp:
    exe = Path(temp) / ("language_locale_test.exe" if os.name == "nt" else "language_locale_test")
    subprocess.run(
        command + [
            "-std=c++20", "-O1", "-pthread", "-I", str(ROOT / "runtime/include"), "-I", str(ROOT / "runtime/src"),
            str(ROOT / "tests/language_locale_test.cpp"), "-o", str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
    print("PASS language_locale", flush=True)
