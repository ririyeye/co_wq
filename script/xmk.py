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
- 若当前 xmake 版本支持 ``--network`` 选项，构建流程会默认使用
    ``--network=private`` 阻止它在缓存目录外执行 ``git checkout`` 或远程拉取；
    若需要更新依赖，可显式传入 ``--online`` 切换为常规联网模式。
- 在执行构建前会检查 ``.xmake/repositories`` 下的依赖仓库，若检测到误指向
    当前项目会自动改回官方镜像（可通过环境变量 ``CO_WQ_XMAKE_REPO_MIRROR``
    指定 `gitee`/`github` 偏好）。
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

EXPECTED_XMAKE_REPOS = {
    "build-artifacts": {
        "urls": (
            "https://gitee.com/xmake-mirror/build-artifacts.git",
            "https://github.com/xmake-mirror/build-artifacts.git",
        ),
        "branch": "main",
    },
    "xmake-repo": {
        "urls": (
            "https://gitee.com/tboox/xmake-repo.git",
            "https://github.com/xmake-io/xmake-repo.git",
        ),
        "branch": "master",
    },
}


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


def _select_repo_url(urls: tuple[str, ...]) -> str:
    preference = os.environ.get("CO_WQ_XMAKE_REPO_MIRROR", "").strip().lower()
    if preference:
        for url in urls:
            if preference in url:
                return url
    for keyword in ("gitee", "github"):
        for url in urls:
            if keyword in url:
                return url
    return urls[0]


def _ensure_xmake_repositories(env: dict[str, str]) -> None:
    for name, info in EXPECTED_XMAKE_REPOS.items():
        urls = tuple(info["urls"])
        expected_branch = str(info["branch"])
        target_url = _select_repo_url(urls)

        legacy_dir = PROJECT_ROOT / ".xmake" / "repositories" / name
        if legacy_dir.exists():
            print(f"Removing legacy xmake repo directory: {legacy_dir}")
            shutil.rmtree(legacy_dir, ignore_errors=True)

        run_command(
            [
                "xmake",
                "repo",
                "--remove",
                name,
            ],
            cwd=PROJECT_ROOT,
            env=env,
            check=False,
        )
        run_command(
            [
                "xmake",
                "repo",
                "--add",
                name,
                target_url,
                expected_branch,
            ],
            cwd=PROJECT_ROOT,
            env=env,
            check=False,
        )


def configure_and_build(mode: str, build_mode: str, iterator_debug: bool, network_mode: str | None) -> None:
    env = os.environ.copy()
    env["XMAKE_GLOBALDIR"] = str(PROJECT_ROOT)

    _ensure_xmake_repositories(env)

    network_flag: list[str] = []
    if network_mode and _xmake_supports_network_flag():
        network_flag = [f"--network={network_mode}"]

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
    run_command(["xmake", *network_flag, "project", "-k", "compile_commands"], cwd=PROJECT_ROOT, env=env)
    run_command(["xmake", *network_flag, "-vD"], cwd=PROJECT_ROOT, env=env)
    run_command(["xmake", *network_flag, "install", "-o", "install"], cwd=PROJECT_ROOT, env=env)


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
    network_mode = getattr(args, "network_mode", None)
    configure_and_build(args.profile, args.mode, args.msvc_iterator_debug, network_mode)


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
