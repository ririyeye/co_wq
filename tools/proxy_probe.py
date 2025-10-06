#!/usr/bin/env python3
"""Proxy probing utility for co_wq HTTP forward proxy.

This script issues HTTP/1.1 requests through a proxy and reports success/timeout
statistics.  It works outside the C++ runtime so that potential workqueue stalls
or coroutine bugs can be surfaced from an independent implementation.
"""
from __future__ import annotations

import argparse
import asyncio
import dataclasses
import errno
import json
import socket
import ssl
import sys
import time
from http.client import HTTPException, HTTPSConnection
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple
from urllib.parse import urlparse


@dataclasses.dataclass
class UrlTarget:
    original: str
    scheme: str
    host: str
    port: int
    path: str


@dataclasses.dataclass
class ConnectTarget:
    original: str
    host: str
    port: int


@dataclasses.dataclass
class RequestConfig:
    proxy_host: str
    proxy_port: int
    timeout: float
    mode: str
    user_agent: str
    verbose: bool
    use_proxy: bool


@dataclasses.dataclass
class RequestResult:
    success: bool
    reason: str
    detail: str
    status_code: Optional[int]
    latency_ms: float
    target_name: str


@dataclasses.dataclass
class AggregateStats:
    attempts: int = 0
    successes: int = 0
    timeouts: int = 0
    connect_errors: int = 0
    send_errors: int = 0
    recv_errors: int = 0
    http_errors: int = 0
    tls_errors: int = 0
    other_errors: int = 0
    total_latency_ms: float = 0.0
    max_latency_ms: float = 0.0
    failure_samples: List[str] = dataclasses.field(default_factory=list)

    def record(self, result: RequestResult) -> None:
        self.attempts += 1
        self.total_latency_ms += result.latency_ms
        if result.latency_ms > self.max_latency_ms:
            self.max_latency_ms = result.latency_ms
        if result.success:
            self.successes += 1
            return
        reason = result.reason
        if reason == "timeout":
            self.timeouts += 1
        elif reason == "connect_error":
            self.connect_errors += 1
        elif reason == "send_error":
            self.send_errors += 1
        elif reason == "recv_error":
            self.recv_errors += 1
        elif reason == "http_error":
            self.http_errors += 1
        elif reason == "tls_error":
            self.tls_errors += 1
        else:
            self.other_errors += 1
        if len(self.failure_samples) < 10:
            if result.status_code is not None:
                sample = f"{result.target_name} -> {result.reason} ({result.status_code}): {result.detail}"
            else:
                sample = f"{result.target_name} -> {result.reason}: {result.detail}"
            self.failure_samples.append(sample)


@dataclasses.dataclass
class PerTargetStats:
    attempts: int = 0
    successes: int = 0
    failures: int = 0
    timeouts: int = 0

    def record(self, result: RequestResult) -> None:
        self.attempts += 1
        if result.success:
            self.successes += 1
        elif result.reason == "timeout":
            self.timeouts += 1
        else:
            self.failures += 1


def load_lines(path: Path) -> List[str]:
    lines: List[str] = []
    with path.open("r", encoding="utf-8") as fh:
        for raw in fh:
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                continue
            lines.append(stripped)
    return lines


def parse_url_target(url: str) -> UrlTarget:
    parsed = urlparse(url)
    scheme = parsed.scheme.lower()
    if scheme not in {"http", "https"}:
        raise ValueError(f"only http:// or https:// URLs are supported, got: {url}")
    if not parsed.hostname:
        raise ValueError(f"missing host in URL: {url}")
    host = parsed.hostname
    default_port = 80 if scheme == "http" else 443
    port = parsed.port or default_port
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"
    return UrlTarget(original=url, scheme=scheme, host=host, port=port, path=path)


def parse_connect_target(raw: str) -> ConnectTarget:
    host = raw
    port = 443
    if raw.startswith("[") and raw.endswith("]"):
        host = raw[1:-1]
    if raw.count(":") == 0:
        port = 443
    elif raw.count(":") == 1 and not raw.startswith("["):
        host_part, port_part = raw.split(":", 1)
        host = host_part
        try:
            port = int(port_part)
        except ValueError as exc:  # pragma: no cover - defensive
            raise ValueError(f"invalid port in CONNECT target: {raw}") from exc
    elif raw.startswith("["):
        closing = raw.find("]:")
        if closing == -1:
            port = 443
        else:
            host = raw[1:closing]
            try:
                port = int(raw[closing + 2 :])
            except ValueError as exc:
                raise ValueError(f"invalid port in CONNECT target: {raw}") from exc
    else:
        raise ValueError(f"CONNECT target must be host:port, got: {raw}")
    if not host:
        raise ValueError(f"CONNECT target missing host: {raw}")
    return ConnectTarget(original=raw, host=host, port=port)


def _format_host_header(host: str, port: int, default_port: int) -> str:
    header = host
    if ":" in header and not header.startswith("["):
        header = f"[{header}]"
    if port != default_port:
        header = f"{header}:{port}"
    return header


def _classify_os_error(exc: OSError) -> str:
    if isinstance(exc, ConnectionRefusedError):
        return "connect_error"
    if isinstance(exc, ConnectionResetError):
        return "recv_error"
    if isinstance(exc, BrokenPipeError):
        return "send_error"
    if isinstance(exc, TimeoutError):  # pragma: no cover - defensive
        return "timeout"
    if exc.errno in {errno.ECONNREFUSED, errno.ENETUNREACH, errno.EHOSTUNREACH}:
        return "connect_error"
    if exc.errno in {errno.ECONNRESET, errno.EPIPE}:
        return "recv_error"
    return "other_error"


async def perform_http_request(cfg: RequestConfig, target: UrlTarget) -> RequestResult:
    if target.scheme != "http":
        raise ValueError("perform_http_request expects an http:// target")
    loop = asyncio.get_running_loop()
    start = loop.time()
    target_name = target.original
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    status_line: Optional[str] = None
    status_code: Optional[int] = None
    try:
        connect_host = cfg.proxy_host if cfg.use_proxy else target.host
        connect_port = cfg.proxy_port if cfg.use_proxy else target.port
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(connect_host, connect_port),
            timeout=cfg.timeout,
        )
    except asyncio.TimeoutError:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "timeout", "connect timeout", None, latency, target_name)
    except OSError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "connect_error", str(exc), None, latency, target_name)

    try:
        host_header = _format_host_header(target.host, target.port, 80)
        request_target = target.original if cfg.use_proxy else target.path
        request_lines = [
            f"GET {request_target} HTTP/1.1",
            f"Host: {host_header}",
            f"User-Agent: {cfg.user_agent}",
            "Accept: */*",
            "Accept-Encoding: identity",
            "Connection: close",
        ]
        if cfg.use_proxy:
            request_lines.append("Proxy-Connection: close")
        request = "\r\n".join(request_lines) + "\r\n\r\n"
        writer.write(request.encode("ascii", errors="ignore"))
        await asyncio.wait_for(writer.drain(), timeout=cfg.timeout)

        status_bytes = await asyncio.wait_for(reader.readline(), timeout=cfg.timeout)
        if not status_bytes:
            raise ConnectionError("proxy closed connection before response")
        status_line = status_bytes.decode("iso-8859-1", errors="replace").strip()
        parts = status_line.split()
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            raise ValueError(f"invalid status line: {status_line}")
        status_code = int(parts[1])
        latency = (loop.time() - start) * 1000.0
        if 200 <= status_code < 400:
            return RequestResult(True, "success", status_line, status_code, latency, target_name)
        return RequestResult(False, "http_error", status_line, status_code, latency, target_name)
    except asyncio.TimeoutError:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "timeout", "operation timed out", status_code, latency, target_name)
    except ValueError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "http_error", str(exc), status_code, latency, target_name)
    except ConnectionError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "recv_error", str(exc), status_code, latency, target_name)
    except OSError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "recv_error", str(exc), status_code, latency, target_name)
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:  # pragma: no cover - best effort cleanup
            pass


async def perform_https_request(cfg: RequestConfig, target: UrlTarget) -> RequestResult:
    if target.scheme != "https":
        raise ValueError("perform_https_request expects an https:// target")
    loop = asyncio.get_running_loop()
    start = loop.time()
    target_name = target.original

    def sync_request() -> Tuple[bool, str, str, Optional[int]]:
        context = ssl.create_default_context()
        if cfg.use_proxy:
            conn = HTTPSConnection(cfg.proxy_host, cfg.proxy_port, timeout=cfg.timeout, context=context)
            conn.set_tunnel(target.host, target.port)
        else:
            conn = HTTPSConnection(target.host, target.port, timeout=cfg.timeout, context=context)
        headers = {
            "Host": _format_host_header(target.host, target.port, 443),
            "User-Agent": cfg.user_agent,
            "Accept": "*/*",
            "Accept-Encoding": "identity",
            "Connection": "close",
        }
        if cfg.use_proxy:
            headers["Proxy-Connection"] = "close"
        try:
            conn.request("GET", target.path, headers=headers)
            response = conn.getresponse()
            detail = f"{response.status} {response.reason}"
            status_code = response.status
            response.read()
            if 200 <= status_code < 400:
                return True, "success", detail, status_code
            return False, "http_error", detail, status_code
        except socket.timeout:
            return False, "timeout", "operation timed out", None
        except ssl.SSLError as exc:
            message = str(exc) or exc.__class__.__name__
            return False, "tls_error", message, None
        except HTTPException as exc:
            message = str(exc) or exc.__class__.__name__
            return False, "http_error", message, None
        except OSError as exc:
            reason = _classify_os_error(exc)
            message = str(exc) or exc.__class__.__name__
            return False, reason, message, None
        finally:
            try:
                conn.close()
            except Exception:  # pragma: no cover - best effort cleanup
                pass

    success, reason, detail, status_code = await asyncio.to_thread(sync_request)
    latency = (loop.time() - start) * 1000.0
    return RequestResult(success, reason, detail, status_code, latency, target_name)


async def perform_connect_request(cfg: RequestConfig, target: ConnectTarget) -> RequestResult:
    if not cfg.use_proxy:
        raise ValueError("CONNECT mode requires a proxy")
    loop = asyncio.get_running_loop()
    start = loop.time()
    target_name = target.original
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    status_code: Optional[int] = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(cfg.proxy_host, cfg.proxy_port),
            timeout=cfg.timeout,
        )
    except asyncio.TimeoutError:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "timeout", "connect timeout", None, latency, target_name)
    except OSError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "connect_error", str(exc), None, latency, target_name)

    try:
        host_field = target.host
        if ":" in host_field and not host_field.startswith("["):
            host_field = f"[{host_field}]"
        if target.port:
            host_field = f"{host_field}:{target.port}"
        request = (
            f"CONNECT {target.host}:{target.port} HTTP/1.1\r\n"
            f"Host: {host_field}\r\n"
            "Proxy-Connection: keep-alive\r\n"
            "Connection: keep-alive\r\n\r\n"
        )
        writer.write(request.encode("ascii", errors="ignore"))
        await asyncio.wait_for(writer.drain(), timeout=cfg.timeout)

        status_bytes = await asyncio.wait_for(reader.readline(), timeout=cfg.timeout)
        if not status_bytes:
            raise ConnectionError("proxy closed connection before response")
        status_line = status_bytes.decode("iso-8859-1", errors="replace").strip()
        parts = status_line.split()
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            raise ValueError(f"invalid status line: {status_line}")
        status_code = int(parts[1])
        latency = (loop.time() - start) * 1000.0
        if 200 <= status_code < 300:
            return RequestResult(True, "success", status_line, status_code, latency, target_name)
        return RequestResult(False, "http_error", status_line, status_code, latency, target_name)
    except asyncio.TimeoutError:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "timeout", "operation timed out", status_code, latency, target_name)
    except ValueError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "http_error", str(exc), status_code, latency, target_name)
    except ConnectionError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "recv_error", str(exc), status_code, latency, target_name)
    except OSError as exc:
        latency = (loop.time() - start) * 1000.0
        return RequestResult(False, "recv_error", str(exc), status_code, latency, target_name)
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:  # pragma: no cover
            pass


async def run_probe(
    cfg: RequestConfig,
    http_targets: Sequence[UrlTarget],
    https_targets: Sequence[UrlTarget],
    connect_targets: Sequence[ConnectTarget],
    per_target_count: int,
    concurrency: int,
) -> Tuple[AggregateStats, List[PerTargetStats]]:
    if cfg.mode == "http":
        targets: Sequence[UrlTarget | ConnectTarget] = http_targets
    elif cfg.mode == "https":
        targets = https_targets
    else:
        targets = connect_targets

    if not targets:
        raise RuntimeError("no targets to probe")

    total_requests = len(targets) * per_target_count
    stats = AggregateStats()
    per_target_stats = [PerTargetStats() for _ in targets]

    queue: asyncio.Queue[Tuple[int, int]] = asyncio.Queue()
    for idx, _ in enumerate(targets):
        for rep in range(per_target_count):
            queue.put_nowait((idx, rep))

    completed_requests = 0
    progress_lock = asyncio.Lock()
    progress_written = False

    async def record_progress() -> None:
        nonlocal completed_requests, progress_written
        completed_requests += 1
        percent = 100.0 * completed_requests / total_requests if total_requests else 100.0
        if cfg.verbose:
            print(
                f"[progress] {completed_requests}/{total_requests} ({percent:.1f}%)",
                flush=True,
            )
        else:
            sys.stdout.write(
                f"\rProgress: {completed_requests}/{total_requests} ({percent:.1f}%)"
            )
            sys.stdout.flush()
            progress_written = True

    async def worker(worker_id: int) -> None:
        while True:
            try:
                target_index, repetition = await queue.get()
            except asyncio.CancelledError:
                break
            target = targets[target_index]
            if cfg.verbose:
                print(f"[worker {worker_id}] ({target_index}:{repetition}) {target.original}")
            try:
                if cfg.mode == "http":
                    result = await perform_http_request(cfg, target)  # type: ignore[arg-type]
                elif cfg.mode == "https":
                    result = await perform_https_request(cfg, target)  # type: ignore[arg-type]
                else:
                    result = await perform_connect_request(cfg, target)  # type: ignore[arg-type]
                stats.record(result)
                per_target_stats[target_index].record(result)
                async with progress_lock:
                    await record_progress()
            finally:
                queue.task_done()

    workers = [asyncio.create_task(worker(i)) for i in range(concurrency)]
    await queue.join()
    for task in workers:
        task.cancel()
    await asyncio.gather(*workers, return_exceptions=True)
    if progress_written and not cfg.verbose:
        print()
    return stats, per_target_stats


def load_config_file(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"configuration root must be an object: {path}")
    return data


def _get_config_value(data: Dict[str, Any], path: Tuple[str, ...]) -> Any:
    current: Any = data
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current


def apply_config_overrides(
    args: argparse.Namespace,
    defaults: argparse.Namespace,
    config: Dict[str, Any],
    config_path: Path,
) -> None:
    base_dir = config_path.parent

    def coerce_bool(value: Any) -> bool:
        if isinstance(value, bool):
            return value
        if isinstance(value, str):
            lowered = value.strip().lower()
            if lowered in {"1", "true", "yes", "on"}:
                return True
            if lowered in {"0", "false", "no", "off"}:
                return False
        raise ValueError("boolean configuration value must be true/false")

    def set_if_default(attr: str, *paths: Tuple[str, ...], transform=None) -> None:
        current = getattr(args, attr)
        default_value = getattr(defaults, attr)
        if current != default_value:
            return
        for path in paths:
            value = _get_config_value(config, path)
            if value is not None:
                if transform is not None:
                    value = transform(value)
                setattr(args, attr, value)
                return

    def extend_list(attr: str, *paths: Tuple[str, ...]) -> None:
        values = list(getattr(args, attr))
        for path in paths:
            cfg_value = _get_config_value(config, path)
            if cfg_value is None:
                continue
            if not isinstance(cfg_value, list):
                raise ValueError(f"configuration field {'.'.join(path)} must be an array")
            values = [str(item) for item in cfg_value] + values
        setattr(args, attr, values)

    def resolve_path(value: Any) -> Optional[Path]:
        if value is None:
            return None
        if isinstance(value, Path):
            return value
        if not isinstance(value, str):
            raise ValueError("path value must be a string")
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = (base_dir / candidate).resolve()
        return candidate

    set_if_default("proxy_host", ("proxy", "host"), ("proxy_host",))
    set_if_default("proxy_port", ("proxy", "port"), ("proxy_port",), transform=int)
    set_if_default("mode", ("mode",), transform=lambda v: str(v).lower())
    set_if_default("timeout", ("timeout",), transform=float)
    set_if_default("user_agent", ("user_agent",))
    set_if_default("count", ("count",), transform=int)
    set_if_default("concurrency", ("concurrency",), transform=int)
    set_if_default("verbose", ("verbose",), transform=coerce_bool)
    set_if_default("compare_direct", ("compare_direct",), transform=coerce_bool)

    urls_file = _get_config_value(config, ("urls_file",))
    targets_file = _get_config_value(config, ("targets_file",))
    if getattr(args, "urls_file") is None and urls_file is not None:
        setattr(args, "urls_file", resolve_path(urls_file))
    if getattr(args, "targets_file") is None and targets_file is not None:
        setattr(args, "targets_file", resolve_path(targets_file))

    extend_list("url", ("urls",))
    extend_list("target", ("targets",), ("connect_targets",))


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Probe co_wq HTTP proxy stability using raw Python requests")
    parser.add_argument("--config", type=Path, help="JSON configuration file")
    parser.add_argument("--proxy-host", default="127.0.0.1", help="proxy host (default: 127.0.0.1)")
    parser.add_argument("--proxy-port", type=int, default=8081, help="proxy port (default: 8081)")
    parser.add_argument("--timeout", type=float, default=5.0, help="per-request timeout in seconds (default: 5.0)")
    parser.add_argument("--mode", choices=["http", "https", "connect"], default="http", help="test mode")
    parser.add_argument("--url", action="append", default=[], help="HTTP(S) URL to request (repeatable)")
    parser.add_argument("--urls-file", type=Path, help="file with HTTP(S) URLs (one per line)")
    parser.add_argument("--target", action="append", default=[], help="CONNECT target host:port (repeatable)")
    parser.add_argument("--targets-file", type=Path, help="file with CONNECT targets (one per line)")
    parser.add_argument("--count", type=int, default=1, help="requests per target (default: 1)")
    parser.add_argument("--concurrency", type=int, default=4, help="number of concurrent workers (default: 4)")
    parser.add_argument("--user-agent", default="co_wq-proxy-probe/0.1", help="override User-Agent header")
    parser.add_argument("--verbose", action="store_true", help="print per-request progress")
    parser.add_argument(
        "--compare-direct",
        action="store_true",
        help="also perform direct requests without proxy for comparison (HTTP/HTTPS only)",
    )
    return parser


def load_targets(args: argparse.Namespace) -> Tuple[List[UrlTarget], List[UrlTarget], List[ConnectTarget]]:
    http_targets: List[UrlTarget] = []
    https_targets: List[UrlTarget] = []
    connect_targets: List[ConnectTarget] = []

    url_entries: List[str] = []
    if args.urls_file:
        if not args.urls_file.exists():
            raise FileNotFoundError(f"URLs file not found: {args.urls_file}")
        url_entries.extend(load_lines(args.urls_file))
    url_entries.extend(str(url) for url in args.url)

    for entry in url_entries:
        target = parse_url_target(entry)
        if target.scheme == "http":
            http_targets.append(target)
        else:
            https_targets.append(target)

    if args.targets_file:
        if not args.targets_file.exists():
            raise FileNotFoundError(f"targets file not found: {args.targets_file}")
        connect_targets.extend(parse_connect_target(line) for line in load_lines(args.targets_file))
    connect_targets.extend(parse_connect_target(str(entry)) for entry in args.target)
    return http_targets, https_targets, connect_targets


def print_summary(
    cfg: RequestConfig,
    stats: AggregateStats,
    per_target: List[PerTargetStats],
    http_targets: Sequence[UrlTarget],
    https_targets: Sequence[UrlTarget],
    connect_targets: Sequence[ConnectTarget],
    count: int,
) -> None:
    if stats.attempts == 0:
        print("No requests executed.")
        return
    success_rate = 100.0 * stats.successes / stats.attempts
    timeout_rate = 100.0 * stats.timeouts / stats.attempts
    avg_latency = stats.total_latency_ms / stats.attempts if stats.attempts else 0.0
    if cfg.mode == "http":
        target_count = len(http_targets)
    elif cfg.mode == "https":
        target_count = len(https_targets)
    else:
        target_count = len(connect_targets)

    title = "Proxy Probe Summary" if cfg.use_proxy else "Direct Request Summary"
    print(f"\n=== {title} ===")
    print(f"Mode: {cfg.mode.upper()}")
    if cfg.use_proxy:
        print(f"Proxy: {cfg.proxy_host}:{cfg.proxy_port}")
    else:
        print("Proxy: (direct connection)")
    print(f"Targets: {target_count} x {count} = {target_count * count} requests")
    print(f"Success: {stats.successes} ({success_rate:.2f}%)")
    print(f"Timeouts: {stats.timeouts} ({timeout_rate:.2f}%)")
    print(f"Connect errors: {stats.connect_errors}")
    print(f"Send errors: {stats.send_errors}")
    print(f"Recv errors: {stats.recv_errors}")
    print(f"HTTP errors: {stats.http_errors}")
    if cfg.mode == "https":
        print(f"TLS errors: {stats.tls_errors}")
    print(f"Other errors: {stats.other_errors}")
    print(f"Average latency: {avg_latency:.2f} ms, max latency: {stats.max_latency_ms:.2f} ms")

    if stats.failure_samples:
        print("\nSample failures:")
        for item in stats.failure_samples:
            print(f"  - {item}")

    problematic = []
    for idx, entry in enumerate(per_target):
        if entry.failures or entry.timeouts:
            problematic.append((idx, entry))
    if problematic:
        problematic.sort(key=lambda pair: pair[1].failures + pair[1].timeouts, reverse=True)
        print("\nWorst targets:")
        for idx, entry in problematic[:5]:
            if cfg.mode == "http":
                name = http_targets[idx].original
            elif cfg.mode == "https":
                name = https_targets[idx].original
            else:
                name = connect_targets[idx].original
            success_ratio = 100.0 * entry.successes / entry.attempts if entry.attempts else 0.0
            summary_bits = [f"{entry.successes}/{entry.attempts} success ({success_ratio:.1f}%)"]
            if entry.timeouts:
                summary_bits.append(f"timeouts={entry.timeouts}")
            if entry.failures:
                summary_bits.append(f"failures={entry.failures}")
            print(f"  - {name}: {', '.join(summary_bits)}")


async def async_main(args: argparse.Namespace) -> int:
    if args.count <= 0:
        print("--count must be positive", file=sys.stderr)
        return 2
    if args.concurrency <= 0:
        print("--concurrency must be positive", file=sys.stderr)
        return 2

    http_targets, https_targets, connect_targets = load_targets(args)
    if args.mode == "http" and not http_targets:
        print("No HTTP URLs specified", file=sys.stderr)
        return 2
    if args.mode == "https" and not https_targets:
        print("No HTTPS URLs specified", file=sys.stderr)
        return 2
    if args.mode == "connect" and not connect_targets:
        print("No CONNECT targets specified", file=sys.stderr)
        return 2

    cfg = RequestConfig(
        proxy_host=args.proxy_host,
        proxy_port=args.proxy_port,
        timeout=args.timeout,
        mode=args.mode,
        user_agent=args.user_agent,
        verbose=args.verbose,
        use_proxy=True,
    )

    stats, per_target = await run_probe(
        cfg,
        http_targets,
        https_targets,
        connect_targets,
        args.count,
        args.concurrency,
    )
    print_summary(cfg, stats, per_target, http_targets, https_targets, connect_targets, args.count)

    if args.compare_direct:
        if args.mode == "connect":
            print("\n[warning] Direct comparison is not available for CONNECT mode.")
        else:
            direct_cfg = dataclasses.replace(cfg, use_proxy=False)
            direct_stats, direct_per_target = await run_probe(
                direct_cfg,
                http_targets,
                https_targets,
                connect_targets,
                args.count,
                args.concurrency,
            )
            print_summary(
                direct_cfg,
                direct_stats,
                direct_per_target,
                http_targets,
                https_targets,
                connect_targets,
                args.count,
            )
    return 0 if stats.successes > 0 else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    defaults = parser.parse_args([])
    args = parser.parse_args(argv)
    try:
        if args.config is not None:
            config_path = args.config.expanduser().resolve()
            config_data = load_config_file(config_path)
            apply_config_overrides(args, defaults, config_data, config_path)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
