#!/usr/bin/env python3
"""Build and install the msquic dependency using CMake."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def _env_or_default(name: str, default: str) -> str:
    value = os.environ.get(name)
    return value if value else default


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    msquic_dir = repo_root / "third_party" / "msquic"
    build_dir = Path(_env_or_default("MSQUIC_BUILD_DIR", str(repo_root / "build" / "msquic")))
    install_dir = Path(_env_or_default("MSQUIC_INSTALL_DIR", str(repo_root / "msquic-install")))
    cmake_build_type = _env_or_default("CMAKE_BUILD_TYPE", "Release")
    cmake_generator = _env_or_default("CMAKE_GENERATOR", "Ninja")

    if not msquic_dir.is_dir():
        print(f"msquic 子模块不存在: {msquic_dir}", file=sys.stderr)
        print("请先执行 'git submodule update --init --recursive'", file=sys.stderr)
        return 1

    cmake_configure_cmd = [
        "cmake",
        "-S",
        str(msquic_dir),
        "-B",
        str(build_dir),
        "-G",
        cmake_generator,
        "-DCMAKE_BUILD_TYPE=" + cmake_build_type,
        "-DCMAKE_INSTALL_PREFIX=" + str(install_dir),
        "-DQUIC_BUILD_SHARED=ON",
        "-DQUIC_BUILD_TEST=OFF",
        "-DQUIC_BUILD_TOOLS=OFF",
    ]

    cmake_build_cmd = [
        "cmake",
        "--build",
        str(build_dir),
        "--config",
        cmake_build_type,
    ]

    cmake_install_cmd = [
        "cmake",
        "--install",
        str(build_dir),
        "--config",
        cmake_build_type,
    ]

    # Allow CMake to create intermediate directories on demand.
    build_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(cmake_configure_cmd, check=True)
    subprocess.run(cmake_build_cmd, check=True)
    subprocess.run(cmake_install_cmd, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
