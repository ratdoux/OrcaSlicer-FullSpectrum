#!/usr/bin/env python3
"""Configure, build, and run the local CTest suite."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=str(cwd), check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Configure, build, and run SnapmakerOrca tests via CMake/CTest."
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="CMake build directory relative to the repository root.",
    )
    parser.add_argument(
        "--build-type",
        default="Release",
        help="CMAKE_BUILD_TYPE used while configuring single-config generators.",
    )
    parser.add_argument(
        "--config",
        default="Release",
        help="CTest/CMake configuration for multi-config generators.",
    )
    parser.add_argument(
        "--target",
        default="tests",
        help="CMake build target to compile before running tests.",
    )
    parser.add_argument(
        "--parallel",
        default=None,
        help="Optional parallel build level passed to cmake --build.",
    )
    parser.add_argument(
        "--ctest-regex",
        default=None,
        help="Optional regular expression passed to ctest -R.",
    )
    parser.add_argument(
        "--catch-extra-args",
        default=None,
        help="Extra Catch2 arguments configured through CATCH_EXTRA_ARGS.",
    )
    parser.add_argument(
        "--skip-configure",
        action="store_true",
        help="Do not run the CMake configure step.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Do not build the test target before running CTest.",
    )
    parser.add_argument(
        "--timeout",
        default=None,
        help="Optional per-test timeout passed to ctest --timeout.",
    )
    parser.add_argument(
        "ctest_args",
        nargs=argparse.REMAINDER,
        help="Additional arguments passed to ctest after --.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = repo_root / build_dir

    if not args.skip_configure:
        configure = [
            "cmake",
            "-S",
            str(repo_root),
            "-B",
            str(build_dir),
            f"-DCMAKE_BUILD_TYPE={args.build_type}",
        ]
        if args.catch_extra_args is not None:
            configure.append(f"-DCATCH_EXTRA_ARGS={args.catch_extra_args}")
        run(configure, repo_root)

    if not args.skip_build:
        build = [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            args.target,
            "--config",
            args.config,
        ]
        if args.parallel is not None:
            build.extend(["--parallel", args.parallel])
        run(build, repo_root)

    ctest = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "-C",
        args.config,
        "--output-on-failure",
    ]
    if args.ctest_regex is not None:
        ctest.extend(["-R", args.ctest_regex])
    if args.timeout is not None:
        ctest.extend(["--timeout", args.timeout])
    extra_ctest_args = args.ctest_args
    if extra_ctest_args and extra_ctest_args[0] == "--":
        extra_ctest_args = extra_ctest_args[1:]
    ctest.extend(extra_ctest_args)
    run(ctest, repo_root)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as err:
        raise SystemExit(err.returncode)
