#!/usr/bin/env python3
"""Create a results directory for an experiment run and record its environment.

    python tools/record_env.py E001
    -> experiments/results/E001/<short-sha>/environment.txt   (prints the directory)

This is step 3 of the protocol in experiments/README.md: every run records the
commit, whether the tree was dirty, the compiler and build type, the machine
and the date, so a committed result can always be traced back to its code.

The run is refused on a dirty tree unless --allow-dirty is given, because the
SHA would then not describe the code that produced the result.

Standard library only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout.strip()


def cmake_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return {}
    wanted = ("CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_VERSION", "CMAKE_BUILD_TYPE",
              "CMAKE_GENERATOR", "CMAKE_CXX_FLAGS")
    out = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^([A-Z_]+):[A-Z]+=(.*)$", line)
        if m and m.group(1) in wanted and m.group(2):
            out[m.group(1)] = m.group(2)
    # Visual Studio generators keep the detected compiler out of the cache, in
    # CMakeFiles/<cmake-version>/CMakeCXXCompiler.cmake.
    for detected in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        text = detected.read_text(encoding="utf-8", errors="replace")
        for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_VERSION"):
            m = re.search(r'set\(%s "([^"]*)"\)' % key, text)
            if m and m.group(1) and key not in out:
                out[key] = m.group(1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("experiment", help="experiment id, e.g. E001")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build",
                    help="CMake build directory the engine was built in (default: build/)")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="record even if the working tree has uncommitted changes")
    args = ap.parse_args()

    if not re.fullmatch(r"E\d{3}", args.experiment):
        raise SystemExit("experiment id must look like E001")

    sha = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    if dirty and not args.allow_dirty:
        raise SystemExit("working tree has uncommitted changes; commit first or pass --allow-dirty")

    out_dir = ROOT / "experiments" / "results" / args.experiment / sha[:10]
    out_dir.mkdir(parents=True, exist_ok=True)

    cache = cmake_cache(args.build_dir)
    lines = [
        f"experiment: {args.experiment}",
        f"date_utc: {dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')}",
        f"git_sha: {sha}",
        f"git_branch: {git('rev-parse', '--abbrev-ref', 'HEAD')}",
        f"git_dirty: {str(dirty).lower()}",
        f"os: {platform.platform()}",
        f"machine: {platform.machine()}",
        f"cpu: {platform.processor() or 'unknown'}",
        f"cpu_count: {os.cpu_count()}",
        f"python: {platform.python_version()}",
        f"cmake_generator: {cache.get('CMAKE_GENERATOR', 'unknown')}",
        f"cxx_compiler: {cache.get('CMAKE_CXX_COMPILER', 'unknown')}",
        f"cxx_compiler_version: {cache.get('CMAKE_CXX_COMPILER_VERSION', 'unknown')}",
        # Multi-config generators (Visual Studio) leave CMAKE_BUILD_TYPE empty;
        # pass --config to the build and note it in config.json instead.
        f"build_type: {cache.get('CMAKE_BUILD_TYPE') or 'multi-config (see config.json)'}",
        f"cxx_flags: {cache.get('CMAKE_CXX_FLAGS', '')}",
    ]
    (out_dir / "environment.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(out_dir.relative_to(ROOT).as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
