"""Builds the UE bridge's C tests (port/ue_bridge/tests) for 32-bit Windows,
the target the game is built for.

    python tools/ue_bridge_tests.py [--asan] [filter]

HALO_UE_BRIDGE_CLANG names the compiler (default: clang on PATH).
HALO_UE_BRIDGE_ASAN_CLANG names an x86 clang.exe whose resource directory has
the i386 AddressSanitizer runtime (the VS 2022 component's VC/Tools/Llvm/bin
has it; its x64 sibling does not).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "port" / "ue_bridge"
BUILD = ROOT / "build" / "ue_bridge_tests"

# every source the test binary links; later tasks append theirs
SOURCES = [
    BRIDGE / "ue_bridge_ring.c",
    BRIDGE / "ue_bridge_policy.c",
    BRIDGE / "tests" / "test_main.c",
    BRIDGE / "tests" / "test_format.c",
    BRIDGE / "tests" / "test_ring.c",
    BRIDGE / "tests" / "test_policy.c",
]
LIBRARIES: list[str] = []

FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-std=gnu11",
    "-Wall",
    "-Werror",
    "-O1",
    "-g",
    "-gcodeview",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    f"-I{BRIDGE}",
    f"-I{BRIDGE / 'tests'}",
]


def compiler(asan: bool) -> str:
    if asan:
        cc = os.environ.get("HALO_UE_BRIDGE_ASAN_CLANG", "")
        if not cc:
            raise RuntimeError("set HALO_UE_BRIDGE_ASAN_CLANG to an x86 clang.exe with the i386 ASan runtime")
        return cc
    return os.environ.get("HALO_UE_BRIDGE_CLANG", "clang")


def asan_runtime(cc: str) -> Path:
    resource = subprocess.run([cc, "--print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    dll = Path(resource) / "lib" / "windows" / "clang_rt.asan_dynamic-i386.dll"
    if not dll.is_file():
        raise RuntimeError(f"{cc} has no {dll}")
    return dll


def build(asan: bool = False) -> Path:
    """the test executable, built; raises CalledProcessError when it doesn't compile"""
    cc = compiler(asan)
    out_dir = BUILD / ("asan" if asan else "plain")
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / "ue_bridge_tests.exe"
    flags = FLAGS + (["-fsanitize=address"] if asan else [])
    subprocess.run(
        [cc, *flags, "-fuse-ld=lld", *map(str, SOURCES), "-o", str(exe), *[f"-l{lib}" for lib in LIBRARIES]],
        check=True,
    )
    if asan:
        runtime = asan_runtime(cc)
        shutil.copyfile(runtime, out_dir / runtime.name)
    return exe


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("filter", nargs="?")
    args = parser.parse_args()
    exe = build(args.asan)
    return subprocess.run([str(exe), *([args.filter] if args.filter else [])]).returncode


if __name__ == "__main__":
    sys.exit(main())
