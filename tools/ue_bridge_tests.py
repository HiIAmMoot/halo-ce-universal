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
    BRIDGE / "ue_bridge_load.c",
    BRIDGE / "ue_bridge_policy.c",
    BRIDGE / "ue_bridge_codec.c",
    BRIDGE / "ue_bridge_model.c",
    BRIDGE / "ue_bridge_bsp.c",
    BRIDGE / "ue_bridge_tick.c",
    ROOT / "port" / "linux" / "src" / "ue_bridge.c",
    BRIDGE / "tests" / "test_main.c",
    BRIDGE / "tests" / "test_format.c",
    BRIDGE / "tests" / "test_ring.c",
    BRIDGE / "tests" / "test_load.c",
    BRIDGE / "tests" / "test_policy.c",
    BRIDGE / "tests" / "test_frame_rate.c",
    ROOT / "port" / "linux" / "src" / "ue_bridge_game.c",
    ROOT / "port" / "linux" / "src" / "ue_bridge_log.c",
    BRIDGE / "tests" / "test_core.c",
    BRIDGE / "tests" / "test_game.c",
    BRIDGE / "tests" / "test_codec.c",
    BRIDGE / "tests" / "test_model.c",
    BRIDGE / "tests" / "test_bsp.c",
    BRIDGE / "tests" / "test_tick.c",
]
LIBRARIES: list[str] = []

# the role processes for the cross-process tests: the core and the real
# Windows layer, no fakes
ROLE_SOURCES = [
    BRIDGE / "ue_bridge_ring.c",
    BRIDGE / "ue_bridge_load.c",
    BRIDGE / "ue_bridge_policy.c",
    BRIDGE / "ue_bridge_tick.c",
    ROOT / "port" / "linux" / "src" / "ue_bridge.c",
    ROOT / "port" / "windows" / "src" / "win32_ue_bridge.c",
    ROOT / "port" / "linux" / "src" / "ue_bridge_log.c",
    BRIDGE / "tests" / "roles.c",
]
ROLE_LIBRARIES = ["bcrypt", "dbghelp"]

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


def _build(name: str, sources: list[Path], libraries: list[str], asan: bool) -> Path:
    cc = compiler(asan)
    out_dir = BUILD / ("asan" if asan else "plain")
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / f"{name}.exe"
    flags = FLAGS + (["-fsanitize=address"] if asan else [])
    subprocess.run(
        [cc, *flags, "-fuse-ld=lld", *map(str, sources), "-o", str(exe), *[f"-l{lib}" for lib in libraries]],
        check=True,
    )
    if asan:
        runtime = asan_runtime(cc)
        shutil.copyfile(runtime, out_dir / runtime.name)
    return exe


def build(asan: bool = False) -> Path:
    """the unit-test executable, built; raises CalledProcessError when it doesn't compile"""
    return _build("ue_bridge_tests", SOURCES, LIBRARIES, asan)


def build_roles(asan: bool = False) -> Path:
    """ue_bridge_roles.exe, built"""
    return _build("ue_bridge_roles", ROLE_SOURCES, ROLE_LIBRARIES, asan)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("filter", nargs="?")
    args = parser.parse_args()
    exe = build(args.asan)
    return subprocess.run([str(exe), *([args.filter] if args.filter else [])]).returncode


if __name__ == "__main__":
    sys.exit(main())
