#!/usr/bin/env python3
import argparse
import importlib
import json
import ssl
import sys
import time
from http.client import HTTPConnection, HTTPSConnection
from typing import Optional


def _print_result(name: str, ok: bool, detail: str) -> None:
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name}: {detail}")


def run_http1(
    base_host: str, port: int, use_tls: bool, verify_tls: bool, timeout: float
) -> bool:
    conn_cls = HTTPSConnection if use_tls else HTTPConnection
    context: Optional[ssl.SSLContext] = None
    if use_tls:
        context = ssl.create_default_context()
        if not verify_tls:
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
    conn = (
        conn_cls(base_host, port, timeout=timeout, context=context)
        if use_tls
        else conn_cls(base_host, port, timeout=timeout)
    )
    try:
        conn.request("GET", "/")
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        ok = resp.status == 200 and "Hello" in body
        _print_result(
            "HTTP/1 GET /",
            ok,
            f"status={resp.status} body_prefix={body.strip()[:40]!r}",
        )
        if not ok:
            return False

        conn.request("GET", "/health")
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        ok = resp.status == 200 and "OK" in body
        _print_result(
            "HTTP/1 GET /health",
            ok,
            f"status={resp.status} body={body.strip()!r}",
        )
        if not ok:
            return False

        conn.request("GET", "/static/index.html")
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        ok = resp.status == 200 and "<!DOCTYPE" in body
        _print_result(
            "HTTP/1 GET /static/index.html",
            ok,
            f"status={resp.status} body_prefix={body.strip()[:40]!r}",
        )
        if not ok:
            return False

        payload = {"message": "ping", "ts": time.time()}
        conn.request(
            "POST",
            "/echo-json",
            body=json.dumps(payload),
            headers={"Content-Type": "application/json"},
        )
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        ok = resp.status == 200
        _print_result(
            "HTTP/1 POST /echo-json",
            ok,
            f"status={resp.status} body={body.strip()[:80]!r}",
        )
        return ok
    except Exception as exc:  # pragma: no cover
        _print_result("HTTP/1", False, str(exc))
        return False
    finally:
        conn.close()


def run_http2(url: str, verify_tls: bool, timeout: float) -> bool:
    try:
        httpx = importlib.import_module("httpx")
    except ImportError:  # pragma: no cover
        _print_result(
            "HTTP/2", False, "python-httpx not installed; pip install httpx[h2]"
        )
        return False
    try:
        with httpx.Client(
            http2=True, verify=verify_tls, timeout=timeout
        ) as client:
            resp = client.get(f"{url}/")
            ok = resp.status_code == 200 and resp.http_version == "HTTP/2"
            _print_result(
                "HTTP/2 GET /",
                ok,
                f"status={resp.status_code} version={resp.http_version}",
            )
            if not ok:
                return False

            resp = client.get(f"{url}/static/index.html")
            ok = resp.status_code == 200
            _print_result(
                "HTTP/2 GET /static/index.html",
                ok,
                f"status={resp.status_code}",
            )
            if not ok:
                return False

            resp = client.get(f"{url}/static/bili_index.html")
            ok = resp.status_code == 200
            _print_result(
                "HTTP/2 GET /static/bili_index.html",
                ok,
                f"status={resp.status_code}",
            )
            return ok
    except Exception as exc:  # pragma: no cover
        _print_result("HTTP/2", False, str(exc))
        return False


def run_http2_fallback(url: str, verify_tls: bool, timeout: float) -> bool:
    try:
        httpx = importlib.import_module("httpx")
    except ImportError:  # pragma: no cover
        _print_result(
            "HTTP/2 fallback",
            False,
            "python-httpx not installed; pip install httpx[h2]",
        )
        return False

    try:
        with httpx.Client(
            http2=True, verify=verify_tls, timeout=timeout
        ) as client:
            resp = client.get(f"{url}/health")
            ok = resp.status_code == 200 and resp.http_version == "HTTP/1.1"
            detail = f"status={resp.status_code} version={resp.http_version}"
            _print_result("HTTP/2 fallback /health", ok, detail)
            return ok
    except Exception as exc:  # pragma: no cover
        _print_result("HTTP/2 fallback", False, str(exc))
        return False


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Probe co_http server endpoints"
    )
    parser.add_argument("--host", default="127.0.0.1", help="server host")
    parser.add_argument("--port", type=int, default=8080, help="server port")
    parser.add_argument("--tls", action="store_true", help="use https")
    parser.add_argument(
        "--insecure", action="store_true", help="skip TLS verification"
    )
    parser.add_argument(
        "--http2", action="store_true", help="probe HTTP/2 as well"
    )
    parser.add_argument(
        "--fallback-host",
        help="指定一个不支持 HTTP/2 的目标以验证客户端回退逻辑",
    )
    parser.add_argument(
        "--fallback-port",
        type=int,
        default=0,
        help="回退目标端口，缺省时沿用 --port",
    )
    parser.add_argument(
        "--fallback-scheme",
        choices=["http", "https"],
        help="回退目标协议，缺省时与主连接一致",
    )
    parser.add_argument(
        "--timeout", type=float, default=5.0, help="request timeout in seconds"
    )
    args = parser.parse_args()

    scheme = "https" if args.tls else "http"
    base_url = f"{scheme}://{args.host}:{args.port}"

    ok_http1 = run_http1(
        args.host, args.port, args.tls, not args.insecure, args.timeout
    )
    ok_http2 = True
    if args.http2:
        ok_http2 = run_http2(base_url, not args.insecure, args.timeout)

    ok_fallback = True
    if args.fallback_host:
        fb_port = args.fallback_port or args.port
        if args.fallback_scheme:
            fb_scheme = args.fallback_scheme
        else:
            fb_scheme = "https" if args.tls else "http"
        fb_url = f"{fb_scheme}://{args.fallback_host}:{fb_port}"
        ok_fallback = run_http2_fallback(
            fb_url, not args.insecure, args.timeout
        )

    if ok_http1 and ok_http2 and ok_fallback:
        print("All checks passed")
        return 0
    print("Checks failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
