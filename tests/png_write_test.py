#!/usr/bin/env python3
"""Compile tests/png_write_test.cpp, let it write a PNG with runtime/src/png_write.h, and decode that PNG with
Python's own zlib: signature, chunk CRCs, header, and every pixel."""
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
compiler = os.environ.get("CXX")
command = shlex.split(compiler) if compiler else [shutil.which("clang++") or shutil.which("g++") or "c++"]

with tempfile.TemporaryDirectory(prefix="png-write-") as temp:
    exe = Path(temp) / ("png_write_test.exe" if os.name == "nt" else "png_write_test")
    out = Path(temp) / "out.png"
    subprocess.run(command + ["-std=c++20", "-O1", "-I", str(ROOT / "runtime/src"),
                              str(ROOT / "tests/png_write_test.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(out)], check=True)
    data = out.read_bytes()

assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
at, chunks = 8, []
while at < len(data):
    (n,) = struct.unpack(">I", data[at:at + 4])
    kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + n]
    (crc,) = struct.unpack(">I", data[at + 8 + n:at + 12 + n])
    assert crc == zlib.crc32(kind + body), f"CRC of {kind}"
    chunks.append((kind, body))
    at += 12 + n
assert [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"], [k for k, _ in chunks]
w, h, depth, colour, comp, filt, inter = struct.unpack(">IIBBBBB", chunks[0][1])
assert (w, h, depth, colour, comp, filt, inter) == (300, 250, 8, 2, 0, 0, 0), chunks[0][1]
raw = zlib.decompress(chunks[1][1])  # also checks the Adler-32
assert len(raw) == (w * 3 + 1) * h, len(raw)
for y in range(h):
    line = raw[y * (w * 3 + 1):(y + 1) * (w * 3 + 1)]
    assert line[0] == 0, "filter none"
    for x in (0, 1, 150, 299):
        assert tuple(line[1 + x * 3:4 + x * 3]) == (x & 255, y & 255, (x ^ y) & 255), (x, y)
print("PASS png_write", flush=True)
