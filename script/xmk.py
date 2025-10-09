#!/usr/bin/env python3
"""Unified build/clean helper for co_wq.

This script replaces the previous platform-specific shell/batch helpers.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def run_command(command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("$", " ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def configure_and_build(mode: str, build_mode: str, iterator_debug: bool) -> None:
    env = os.environ.copy()
    env["XMAKE_GLOBALDIR"] = str(PROJECT_ROOT)

    config_flags: list[str]
    if mode == "full":
        config_flags = [
            "--USING_NET=y",
            "--USING_SSL=y",
            "--USE_BUNDLED_LLHTTP=y",
            "--USING_USB=y",
            "--ENABLE_LOGGING=y",
            "--USING_EXAMPLE=y",
        ]
    else:
        config_flags = [
            "--USING_NET=n",
            "--USING_SSL=n",
            "--USE_BUNDLED_LLHTTP=n",
            "--USING_USB=n",
            "--ENABLE_LOGGING=n",
            "--USING_EXAMPLE=n",
        ]

    config_flags.append(f"--MSVC_ITERATOR_DEBUG={'y' if iterator_debug else 'n'}")

    configure_cmd = [
        "xmake",
        "f",
        "-y",
        "-vD",
        "-m",
        build_mode,
        "-o",
        "build",
        *config_flags,
    ]

    run_command(configure_cmd, cwd=PROJECT_ROOT, env=env)
    run_command(["xmake", "project", "-k", "compile_commands"], cwd=PROJECT_ROOT, env=env)
    run_command(["xmake", "-vD"], cwd=PROJECT_ROOT, env=env)
    run_command(["xmake", "install", "-o", "install"], cwd=PROJECT_ROOT, env=env)


def clean_workspace(remove_global_cache: bool) -> None:
    targets = [PROJECT_ROOT / name for name in (".xmake", "build", "install", ".cache")]
    for path in targets:
        if path.exists():
            print(f"Removing {path}")
            shutil.rmtree(path, ignore_errors=True)

    if remove_global_cache:
        global_cache = Path.home() / ".xmake"
        if global_cache.exists():
            print(f"Removing {global_cache}")
            shutil.rmtree(global_cache, ignore_errors=True)


def build_command(args: argparse.Namespace) -> None:
    configure_and_build(args.profile, args.mode, args.msvc_iterator_debug)


def clean_command(args: argparse.Namespace) -> None:
    clean_workspace(args.remove_global_cache)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="co_wq build helper")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build", help="Configure and build co_wq")
    build_parser.add_argument(
        "--core",
        dest="profile",
        action="store_const",
        const="core",
        default="full",
        help="Build minimal core (disable net/ssl/usb/examples/logging)",
    )
    build_parser.add_argument(
        "--full",
        dest="profile",
        action="store_const",
        const="full",
        help="Build full feature set (default)",
    )
    build_parser.add_argument(
        "--debug",
        dest="mode",
        action="store_const",
        const="debug",
        default="releasedbg",
        help="Use xmake debug mode",
    )
    build_parser.add_argument(
        "--releasedbg",
        dest="mode",
        action="store_const",
        const="releasedbg",
        help="Use xmake releasedbg mode (default)",
    )
    build_parser.add_argument(
        "--msvc-iterator-debug",
        dest="msvc_iterator_debug",
        action="store_true",
        help="Enable MSVC iterator debug checks",
    )
    build_parser.add_argument(
        "--no-msvc-iterator-debug",
        dest="msvc_iterator_debug",
        action="store_false",
        help="Disable MSVC iterator debug checks",
    )
    build_parser.set_defaults(msvc_iterator_debug=False)
    build_parser.set_defaults(func=build_command)

    clean_parser = subparsers.add_parser("clean", help="Remove build artifacts")
    clean_parser.add_argument(
        "--remove-global-cache",
        action="store_true",
        help="Also remove ~/.xmake cache",
    )
    clean_parser.set_defaults(func=clean_command)

    args = parser.parse_args(argv)
    try:
        args.func(args)
    except subprocess.CalledProcessError as exc:
        print(f"Command failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
