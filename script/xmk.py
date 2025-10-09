#!/usr/bin/env python3
"""co_wq 的统一构建/清理工具。

此 Python 入口将原本分散在各平台 shell/bat 脚本里的构建与清理流程全部收拢。
无论开发者使用哪种操作系统，都可以只记住这一条命令；因此请确保这里的
行为始终与 `README.md` 内的说明保持一致。

整体行为
--------
- 当脚本 **不带任何参数** 执行时，会自动等价为 ``python script/xmk.py build``，
    沿用历史默认（配置 + 构建 + 安装）的 "full" 功能集。
- ``build`` 子命令负责完整的 xmake 配置流程，生成 ``compile_commands.json``，
    编译目标并安装到 ``install/``。命令行参数可切换功能档（``--core``/``--full``）、
    xmake 构建模式（``--debug``/``--releasedbg``）以及 MSVC 迭代器调试开关。
- ``clean`` 子命令清理本地构建产物，如附带 ``--remove-global-cache`` 会额外删除
    用户级别的 ``~/.xmake`` 缓存。

实现提示
--------
- 始终将 ``XMAKE_GLOBALDIR`` 设为仓库根目录，确保 xmake 在本地缓存依赖包而不是
    写到不可预期的位置。
- 解析器的参数定义请与 README 示例同步，很多用户会直接复制那些命令。
- 新增子命令时保持同样的结构：编写一个处理函数（例如 ``*_command``），并通过
    ``set_defaults(func=...)`` 注册到主分发逻辑，使得入口保持精简。

若未来增加新的开关或流程，请同步更新此文档字符串和 README，记录对外约定——
后续贡献者（以及 CI）都依赖它作为唯一可信的接口说明。
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

    if not argv:
        argv = ["build"]

    args = parser.parse_args(argv)
    try:
        args.func(args)
    except subprocess.CalledProcessError as exc:
        print(f"Command failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
