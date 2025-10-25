#!/usr/bin/env python3
"""co_wq 的统一构建/清理工具。

此 Python 入口将原本分散在各平台 shell/bat 脚本里的构建与清理流程全部收拢。
无论开发者使用哪种操作系统，都可以只记住这一条命令；因此请确保这里的
行为始终与 `README.md` 内的说明保持一致。

- 当脚本 **不带任何参数** 执行时，会自动等价为 ``python script/xmk.py build``，
    沿用历史默认（配置 + 构建 + 安装）的 "full" 功能集。
- ``build`` 子命令负责完整的 xmake 配置流程，生成 ``compile_commands.json``，
    编译目标并安装到 ``install/``。命令行参数可切换功能档（``--core``/``--full``）、
    xmake 构建模式（``--debug``/``--releasedbg``）以及 MSVC 迭代器调试开关。
- ``quictest`` 子命令仅为 MsQuic 示例做最小化构建；运行前请确保 ``msquic-install``
    已由 ``script/build_msquic.sh`` 生成，以便动态加载。
- 若当前 xmake 版本支持 ``--network`` 选项，构建流程会默认使用
    ``--network=private`` 阻止它在缓存目录外执行 ``git checkout`` 或远程拉取；
    若需要更新依赖，可显式传入 ``--online`` 切换为常规联网模式。
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
from functools import lru_cache
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent


@lru_cache(maxsize=1)
def _xmake_supports_network_flag() -> bool:
    try:
        result = subprocess.run(
            ["xmake", "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return "--network=" in result.stdout


def run_command(
    command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None, check: bool = True
) -> subprocess.CompletedProcess[bytes] | subprocess.CompletedProcess[str]:
    print("$", " ".join(command))
    return subprocess.run(command, cwd=cwd, env=env, check=check)


def ensure_msquic_installation() -> None:
    install_root = PROJECT_ROOT / "msquic-install"
    if not install_root.is_dir():
        raise FileNotFoundError("未找到 msquic-install 目录，请先构建 msquic")

    lib_dir = install_root / "lib"
    if not lib_dir.is_dir():
        raise FileNotFoundError("msquic-install/lib 不存在")

    has_shared_lib = any(
        candidate.is_file()
        for pattern in ("libmsquic.so", "libmsquic.so.*", "msquic.dll", "libmsquic.dylib")
        for candidate in lib_dir.glob(pattern)
    )
    if not has_shared_lib:
        raise FileNotFoundError("msquic-install/lib 下未找到 libmsquic 动态库")

    include_dir = install_root / "include"
    if not include_dir.is_dir():
        raise FileNotFoundError("msquic-install/include 不存在")

    if not any(path.name == "msquic.h" for path in include_dir.rglob("msquic.h")):
        raise FileNotFoundError("msquic-install/include 下未找到 msquic.h 头文件")


def configure_and_build(
    mode: str,
    build_mode: str,
    iterator_debug: bool,
    network_mode: str | None,
    enable_usb: bool,
    proxy_url: str | None,
    *,
    enable_msquic: bool = False,
    enable_msquic_tests: bool = False,
    build_targets: list[str] | None = None,
    run_install: bool = True,
) -> None:
    env = os.environ.copy()
    env["XMAKE_GLOBALDIR"] = str(PROJECT_ROOT)

    if proxy_url:
        env["HTTP_PROXY"] = proxy_url
        env["HTTPS_PROXY"] = proxy_url
        env["http_proxy"] = proxy_url
        env["https_proxy"] = proxy_url

    network_flag: list[str] = []
    if network_mode and _xmake_supports_network_flag():
        network_flag = [f"--network={network_mode}"]

    if enable_msquic or enable_msquic_tests:
        ensure_msquic_installation()

    config_flags: list[str]
    usb_flag    = "y" if enable_usb else "n"
    msquic_flag = "y" if enable_msquic else "n"
    msquic_test_flag = "y" if enable_msquic_tests else "n"

    if mode == "full":
        config_flags = [
            "--USING_NET=y",
            "--USING_SSL=y",
            "--USE_BUNDLED_LLHTTP=y",
            "--USE_BUNDLED_NGHTTP2=y",
            f"--USING_USB={usb_flag}",
            "--ENABLE_LOGGING=y",
            "--USING_EXAMPLE=y",
            f"--USING_MSQUIC={msquic_flag}",
            f"--USING_MSQUIC_TEST={msquic_test_flag}",
        ]
    else:
        config_flags = [
            "--USING_NET=n",
            "--USING_SSL=n",
            "--USE_BUNDLED_LLHTTP=n",
            "--USE_BUNDLED_NGHTTP2=n",
            f"--USING_USB={usb_flag}",
            "--ENABLE_LOGGING=n",
            "--USING_EXAMPLE=n",
            f"--USING_MSQUIC={msquic_flag}",
            f"--USING_MSQUIC_TEST={msquic_test_flag}",
        ]

    config_flags.append(
        f"--MSVC_ITERATOR_DEBUG={'y' if iterator_debug else 'n'}")

    configure_cmd = [
        "xmake",
        *network_flag,
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
    run_command(["xmake", *network_flag, "project", "-k",
                "compile_commands"], cwd=PROJECT_ROOT, env=env)

    if build_targets:
        for target in build_targets:
            run_command(["xmake", *network_flag, "build", target],
                        cwd=PROJECT_ROOT, env=env)
    else:
        run_command(["xmake", *network_flag, "-vD"],
                    cwd=PROJECT_ROOT, env=env)

    if run_install and not build_targets:
        run_command(["xmake", *network_flag, "install", "-o",
                    "install"], cwd=PROJECT_ROOT, env=env)


def clean_workspace(remove_global_cache: bool) -> None:
    def _handle_remove_error(func, path, _exc):
        try:
            os.chmod(path, 0o700)
        except OSError:
            return
        func(path)

    targets = [PROJECT_ROOT / name for name in (".xmake", "build", "install", ".cache")]
    for path in targets:
        if not path.exists():
            continue
        print(f"Removing {path}")
        if path.is_file() or path.is_symlink():
            try:
                path.unlink()
            except OSError:
                pass
            continue
        shutil.rmtree(path, onerror=_handle_remove_error, ignore_errors=False)

    if remove_global_cache:
        global_cache = Path.home() / ".xmake"
        if global_cache.exists():
            print(f"Removing {global_cache}")
            shutil.rmtree(global_cache, ignore_errors=True)


def build_command(args: argparse.Namespace) -> None:
    network_mode = getattr(args, "network_mode", None)
    configure_and_build(args.profile, args.mode,
                        args.msvc_iterator_debug, network_mode, args.enable_usb, args.proxy_url,
                        enable_msquic=True)


def clean_command(args: argparse.Namespace) -> None:
    clean_workspace(args.remove_global_cache)


def quictest_command(args: argparse.Namespace) -> None:
    network_mode = getattr(args, "network_mode", None)
    try:
        configure_and_build(
            mode="full",
            build_mode=args.mode,
            iterator_debug=args.msvc_iterator_debug,
            network_mode=network_mode,
            enable_usb=False,
            proxy_url=args.proxy_url,
            enable_msquic=True,
            enable_msquic_tests=True,
        )
    except FileNotFoundError as exc:
        print(f"MsQuic 依赖缺失: {exc}", file=sys.stderr)
        print("请先执行 ./script/build_msquic.sh 生成 msquic-install", file=sys.stderr)
        raise SystemExit(1)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="co_wq build helper")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser(
        "build", help="Configure and build co_wq")
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
    build_parser.add_argument(
        "--with-usb",
        dest="enable_usb",
        action="store_true",
        help="Enable USB support (pulls libusb dependency)",
    )
    build_parser.add_argument(
        "--without-usb",
        dest="enable_usb",
        action="store_false",
        help="Disable USB support (default)",
    )
    build_parser.set_defaults(enable_usb=False)
    build_parser.add_argument(
        "--proxy",
        dest="proxy_url",
        metavar="URL",
        default=None,
        help="Set HTTP(S) proxy for child processes",
    )
    if _xmake_supports_network_flag():
        build_parser.add_argument(
            "--offline",
            dest="network_mode",
            action="store_const",
            const="private",
            default="private",
            help="Disable xmake network access (skip package checkouts, default)",
        )
        build_parser.add_argument(
            "--online",
            dest="network_mode",
            action="store_const",
            const="public",
            help="Allow xmake to reach network for repo/package updates",
        )
    else:
        build_parser.set_defaults(network_mode=None)
    build_parser.set_defaults(func=build_command)

    quic_parser = subparsers.add_parser(
        "quictest",
        help="仅构建 MsQuic 相关示例 (co_msquic_echo)",
    )
    quic_parser.add_argument(
        "--debug",
        dest="mode",
        action="store_const",
        const="debug",
        default="releasedbg",
        help="使用 xmake debug 模式",
    )
    quic_parser.add_argument(
        "--releasedbg",
        dest="mode",
        action="store_const",
        const="releasedbg",
        help="使用 xmake releasedbg 模式 (默认)",
    )
    quic_parser.add_argument(
        "--msvc-iterator-debug",
        dest="msvc_iterator_debug",
        action="store_true",
        help="启用 MSVC 迭代器调试检查",
    )
    quic_parser.add_argument(
        "--no-msvc-iterator-debug",
        dest="msvc_iterator_debug",
        action="store_false",
        help="禁用 MSVC 迭代器调试检查 (默认)",
    )
    quic_parser.set_defaults(msvc_iterator_debug=False)
    quic_parser.add_argument(
        "--proxy",
        dest="proxy_url",
        metavar="URL",
        default=None,
        help="为子进程设置 HTTP(S) 代理",
    )
    if _xmake_supports_network_flag():
        quic_parser.add_argument(
            "--offline",
            dest="network_mode",
            action="store_const",
            const="private",
            default="private",
            help="禁用 xmake 联网 (默认)",
        )
        quic_parser.add_argument(
            "--online",
            dest="network_mode",
            action="store_const",
            const="public",
            help="允许 xmake 联网以更新依赖",
        )
    else:
        quic_parser.set_defaults(network_mode=None)
    quic_parser.set_defaults(func=quictest_command)

    clean_parser = subparsers.add_parser(
        "clean", help="Remove build artifacts")
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
        print(
            f"Command failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
