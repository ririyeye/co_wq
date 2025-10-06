#!/usr/bin/env python3
"""Capture proxy traffic using dumpcap.

This script replicates the functionality of capture_proxy.bat with a Python interface.
"""
from __future__ import annotations

import argparse
import codecs
import datetime as dt
import locale
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List

try:
    import dns.resolver  # type: ignore
except ImportError:  # pragma: no cover
    dns = None  # type: ignore
else:
    dns = dns  # type: ignore

DEFAULT_TARGET = "www.baidu.com"
DEFAULT_DURATION = 120
DEFAULT_PROXY_PORT = 18100
DEFAULT_REMOTE_PORT = 443


def resolve_ipv4(host: str) -> List[str]:
    # Prefer dnspython if available for better control.
    if dns is not None:
        try:
            answer = dns.resolver.resolve(host, "A")
            return [r.address for r in answer]
        except Exception:
            return []
    # Fallback to socket.getaddrinfo
    import socket

    try:
        infos = socket.getaddrinfo(host, None, socket.AF_INET, socket.SOCK_STREAM)
    except socket.gaierror:
        return []
    results: List[str] = []
    for info in infos:
        sockaddr = info[4]
        if sockaddr:
            addr = sockaddr[0]
            if addr not in results:
                results.append(addr)
    return results


def build_default_filter(target_ips: List[str]) -> str:
    base = f"tcp port {DEFAULT_PROXY_PORT}"
    if not target_ips:
        return base
    host_conditions = " or ".join(f"host {ip}" for ip in target_ips)
    return f"{base} or (tcp port {DEFAULT_REMOTE_PORT} and ({host_conditions}))"


def find_dumpcap(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"dumpcap not found at {explicit}")

    dumpcap = shutil.which("dumpcap")
    if dumpcap:
        return Path(dumpcap)

    default_path = Path("C:/Program Files/Wireshark/dumpcap.exe")
    if default_path.is_file():
        return default_path

    raise FileNotFoundError("dumpcap executable not found. Install Wireshark or specify --dumpcap path.")


def run_dumpcap(
    dumpcap_path: Path,
    interface: str,
    duration: int,
    capture_filter: str,
    output_path: Path,
    stdout_encoding: str,
) -> int:
    cmd = [
        str(dumpcap_path),
        "-i",
        interface,
        "-a",
        f"duration:{duration}",
        "-f",
        capture_filter,
        "-w",
        str(output_path),
    ]
    print("[capture] command:", " ".join(cmd))
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding=stdout_encoding,
            errors="replace",
        )
    except LookupError:
        print(f"[capture] warning: unknown encoding '{stdout_encoding}', falling back to utf-8")
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

    assert proc.stdout is not None
    with proc:
        for line in proc.stdout:
            print(line.rstrip())
        return proc.wait()


def ensure_log_dir(log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)


def capture_main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Capture proxy traffic via dumpcap")
    parser.add_argument("interface", help="Interface index/name as accepted by dumpcap")
    parser.add_argument("duration", nargs="?", type=int, default=DEFAULT_DURATION, help="Duration in seconds")
    parser.add_argument("--target", default=DEFAULT_TARGET, help="Hostname to resolve for remote capture filter")
    parser.add_argument("--dumpcap", dest="dumpcap_path", help="Explicit dumpcap executable path")
    parser.add_argument("--output", dest="output_path", help="Optional explicit output file path")
    parser.add_argument(
        "--encoding",
        dest="stdout_encoding",
        help="Encoding used to decode dumpcap output (default: system preferred encoding)",
    )

    args = parser.parse_args(argv)

    dumpcap_path = find_dumpcap(args.dumpcap_path)

    project_root = Path(__file__).resolve().parent.parent
    log_dir = (project_root / "logs").resolve()
    ensure_log_dir(log_dir)

    if args.output_path:
        output_path = Path(args.output_path)
        if not output_path.is_absolute():
            output_path = log_dir / output_path
    else:
        timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = log_dir / f"proxy_capture_{timestamp}.pcapng"

    target_ips = resolve_ipv4(args.target)
    capture_filter = build_default_filter(target_ips)
    if not target_ips:
        print(f"[capture] warning: failed to resolve {args.target}. Capturing only local port {DEFAULT_PROXY_PORT}")

    print(f"[capture] dumpcap: {dumpcap_path}")
    print(f"[capture] interface: {args.interface}")
    print(f"[capture] duration: {args.duration}s")
    print(f"[capture] filter: {capture_filter}")
    print(f"[capture] output: {output_path}")

    preferred_encoding = args.stdout_encoding
    if preferred_encoding:
        try:
            codecs.lookup(preferred_encoding)
        except LookupError:
            print(f"[capture] warning: invalid encoding '{preferred_encoding}', falling back to system default")
            preferred_encoding = None

    if preferred_encoding is None:
        preferred_encoding = locale.getpreferredencoding(False) or "utf-8"

    print(f"[capture] output encoding: {preferred_encoding}")

    return_code = run_dumpcap(
        dumpcap_path,
        args.interface,
        args.duration,
        capture_filter,
        output_path,
        preferred_encoding,
    )
    if return_code == 0:
        print("[capture] capture complete.")
    else:
        print(f"[capture] dumpcap exited with code {return_code}")
    return return_code


if __name__ == "__main__":
    sys.exit(capture_main(sys.argv[1:]))
