#!/usr/bin/env python3
"""Async chat stability load client for the co_wq test server."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

FRAME_TYPE_CONTROL = 0x01
FRAME_TYPE_DATA = 0x02
MAX_FRAME_BYTES = 16 * 1024 * 1024


@dataclass
class TLSOptions:
    enabled: bool = False
    verify: bool = True
    ca_file: Optional[str] = None
    cert_file: Optional[str] = None
    key_file: Optional[str] = None


@dataclass
class TransportConfig:
    host: str = "127.0.0.1"
    port: int = 9100
    tls: TLSOptions = field(default_factory=TLSOptions)


@dataclass
class BotPlan:
    client_id: str
    send_plan: int
    payload_bytes: int
    expect_receive: int
    server_push: int
    shutdown_after_send: bool
    room: Optional[str] = None
    mode: str = "pair"


@dataclass
class RuntimeConfig:
    max_concurrency: int = 32
    connect_interval_ms: int = 0
    handshake_timeout_ms: int = 30000
    connect_timeout_ms: int = 10000
    send_delay_ms: int = 0
    log_level: str = "INFO"


@dataclass
class BotResult:
    client_id: str
    room: Optional[str] = None
    status: str = "ok"
    sent: int = 0
    recv_peer: int = 0
    recv_extra: int = 0
    summary: Dict[str, Any] = field(default_factory=dict)
    errors: List[str] = field(default_factory=list)
    server_errors: List[str] = field(default_factory=list)
    duration_sec: float = 0.0

    def mark_error(self, message: str) -> None:
        if message not in self.errors:
            logging.warning("[%s] %s", self.client_id, message)
            self.errors.append(message)
        self.status = "error"

    def merge_summary(self) -> None:
        if not self.summary:
            return
        error_msg = self.summary.get("error")
        if error_msg:
            self.server_errors.append(str(error_msg))
            self.status = "error"


@dataclass
class BotState:
    plan: BotPlan
    result: BotResult
    start_event: asyncio.Event = field(default_factory=asyncio.Event)
    stop_event: asyncio.Event = field(default_factory=asyncio.Event)
    ready_payload: Dict[str, Any] = field(default_factory=dict)
    summary_received: bool = False


def load_json_config(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def discover_config(path_arg: Optional[Path]) -> Path:
    if path_arg:
        candidate = path_arg.expanduser()
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"config file not found: {candidate}")

    search_root = Path.cwd()
    preferred = ["chat_stability.json", "chat_stability.sample.json"]
    for name in preferred:
        candidate = search_root / name
        if candidate.is_file():
            return candidate

    matches = sorted(search_root.glob("chat_stability*.json"))
    if matches:
        return matches[0]

    raise FileNotFoundError(
        f"no configuration file found in {search_root}; specify --config"
    )


def parse_tls_options(data: Dict[str, Any]) -> TLSOptions:
    return TLSOptions(
        enabled=bool(data.get("enabled", False)),
        verify=bool(data.get("verify", True)),
        ca_file=data.get("ca_file"),
        cert_file=data.get("cert"),
        key_file=data.get("key"),
    )


def parse_transport(data: Dict[str, Any]) -> TransportConfig:
    tls = parse_tls_options(data.get("tls", {}))
    return TransportConfig(
        host=data.get("host", "127.0.0.1"),
        port=int(data.get("port", 9100)),
        tls=tls,
    )


def parse_runtime(data: Dict[str, Any]) -> RuntimeConfig:
    return RuntimeConfig(
        max_concurrency=int(data.get("max_concurrency", 32)),
        connect_interval_ms=int(data.get("connect_interval_ms", 0)),
        handshake_timeout_ms=int(data.get("handshake_timeout_ms", 30000)),
        connect_timeout_ms=int(data.get("connect_timeout_ms", 10000)),
        send_delay_ms=int(data.get("send_delay_ms", 0)),
        log_level=str(data.get("log_level", "INFO")),
    )


def derive_bot_plans(config: Dict[str, Any]) -> List[BotPlan]:
    defaults = config.get("defaults", {})
    send_plan_def = int(defaults.get("send_plan", defaults.get("cnt", 0)))
    payload_def = int(defaults.get("payload_bytes", 0))
    expect_def = defaults.get("expect_receive")
    if expect_def is not None:
        expect_def = int(expect_def)
    server_push_def = int(defaults.get("server_push", 0))
    shutdown_def = bool(defaults.get("shutdown_after_send", True))
    mode_def = str(defaults.get("mode", "pair")).lower()
    if defaults.get("solo") is True:
        mode_def = "solo"

    plans: List[BotPlan] = []

    for entry in config.get("workloads", []):
        mode = str(entry.get("mode", mode_def)).lower()
        if entry.get("solo") is True:
            mode = "solo"
        if entry.get("pair") is True:
            mode = "pair"
        if mode not in {"pair", "solo"}:
            mode = "pair"

        send_plan = int(entry.get("send_plan", entry.get("cnt", send_plan_def)))
        payload_bytes = int(entry.get("payload_bytes", payload_def))
        expect_receive_val = entry.get("expect_receive", expect_def)
        if expect_receive_val is None:
            expect_receive = send_plan
        else:
            expect_receive = int(expect_receive_val)
        server_push = int(entry.get("server_push", server_push_def))
        shutdown = bool(entry.get("shutdown_after_send", shutdown_def))

        if mode == "pair":
            rooms = int(entry.get("rooms", 0))
            if rooms <= 0:
                continue
            per_room = int(entry.get("clients_per_room", 2))
            room_prefix = entry.get("room_prefix", "room")
            client_prefix = entry.get("client_id_prefix", "bot")
            start_index = int(entry.get("start_index", 0))
            explicit_room = entry.get("room")
            explicit_client = entry.get("client_id")

            for room_idx in range(start_index, start_index + rooms):
                if explicit_room is not None:
                    room_name = str(explicit_room)
                else:
                    room_name = f"{room_prefix}-{room_idx:04d}"
                for client_idx in range(per_room):
                    if explicit_client:
                        if rooms > 1 or per_room > 1:
                            client_id = f"{explicit_client}-{room_idx:04d}-{client_idx}"
                        else:
                            client_id = str(explicit_client)
                    else:
                        client_id = f"{client_prefix}-{room_idx:04d}-{client_idx}"
                    plans.append(
                        BotPlan(
                            client_id=client_id,
                            send_plan=send_plan,
                            payload_bytes=payload_bytes,
                            expect_receive=expect_receive,
                            server_push=server_push,
                            shutdown_after_send=shutdown,
                            room=room_name,
                            mode=mode,
                        )
                    )
        else:
            total_clients = entry.get("clients")
            if total_clients is None:
                total_clients = entry.get("count")
            if total_clients is None:
                total_clients = entry.get("rooms", 1)
            total_clients = max(1, int(total_clients))
            client_prefix = entry.get("client_id_prefix", "bot")
            room_prefix = entry.get("room_prefix")
            explicit_room = entry.get("room")
            explicit_client = entry.get("client_id")
            start_index = int(entry.get("start_index", 0))

            for offset in range(total_clients):
                seq = start_index + offset
                if explicit_room is not None:
                    room_name = None if explicit_room == "" else str(explicit_room)
                elif room_prefix:
                    room_name = f"{room_prefix}-{seq:04d}"
                else:
                    room_name = None
                if explicit_client:
                    if total_clients > 1:
                        client_id = f"{explicit_client}-{seq:04d}"
                    else:
                        client_id = str(explicit_client)
                else:
                    client_id = f"{client_prefix}-{seq:04d}"
                plans.append(
                    BotPlan(
                        client_id=client_id,
                        send_plan=send_plan,
                        payload_bytes=payload_bytes,
                        expect_receive=expect_receive,
                        server_push=server_push,
                        shutdown_after_send=shutdown,
                        room=room_name if room_name else None,
                        mode=mode,
                    )
                )

    for entry in config.get("bots", []):
        mode = str(entry.get("mode", mode_def)).lower()
        if entry.get("solo") is True:
            mode = "solo"
        if mode not in {"pair", "solo"}:
            mode = "pair"
        room_value = entry.get("room")
        client_id = entry.get("client_id")
        if not client_id:
            raise ValueError("explicit bot entry requires client_id")
        if mode != "solo" and (room_value is None or room_value == ""):
            raise ValueError("pair-mode bot entry requires room")
        send_plan = int(entry.get("send_plan", entry.get("cnt", send_plan_def)))
        payload_bytes = int(entry.get("payload_bytes", payload_def))
        expect_receive_val = entry.get("expect_receive", expect_def)
        if expect_receive_val is None:
            expect_receive = send_plan
        else:
            expect_receive = int(expect_receive_val)
        server_push = int(entry.get("server_push", server_push_def))
        shutdown = bool(entry.get("shutdown_after_send", shutdown_def))
        plans.append(
            BotPlan(
                client_id=str(client_id),
                send_plan=send_plan,
                payload_bytes=payload_bytes,
                expect_receive=expect_receive,
                server_push=server_push,
                shutdown_after_send=shutdown,
                room=None if room_value in (None, "") else str(room_value),
                mode=mode,
            )
        )

    if not plans:
        raise ValueError("config produced no bot plans")
    return plans


def create_ssl_context(tls: TLSOptions) -> Optional[ssl.SSLContext]:
    if not tls.enabled:
        return None
    context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH)
    if not tls.verify:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    if tls.ca_file:
        context.load_verify_locations(tls.ca_file)
    if tls.cert_file:
        context.load_cert_chain(tls.cert_file, tls.key_file)
    return context


def build_data_payload(seq: int, payload_bytes: int) -> bytes:
    body = bytes([(65 + (seq % 26)) & 0xFF]) * max(payload_bytes, 0)
    return struct.pack("!Q", seq) + body


async def write_control(writer: asyncio.StreamWriter, message: Dict[str, Any]) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = struct.pack("!BI", FRAME_TYPE_CONTROL, len(payload))
    writer.write(header + payload)
    await writer.drain()


async def write_data(writer: asyncio.StreamWriter, payload: bytes) -> None:
    header = struct.pack("!BI", FRAME_TYPE_DATA, len(payload))
    writer.write(header + payload)
    await writer.drain()


async def read_frame(reader: asyncio.StreamReader) -> Optional[tuple[int, bytes]]:
    try:
        header = await reader.readexactly(5)
    except asyncio.IncompleteReadError:
        return None
    frame_type, length = struct.unpack("!BI", header)
    if length > MAX_FRAME_BYTES:
        raise ValueError(f"server frame exceeds limit: {length} bytes")
    payload = await reader.readexactly(length)
    return frame_type, payload


async def reader_task(reader: asyncio.StreamReader, state: BotState) -> None:
    result = state.result
    plan = state.plan
    while not state.stop_event.is_set():
        frame = await read_frame(reader)
        if frame is None:
            logging.debug("[%s] connection closed by server", result.client_id)
            break
        frame_type, payload = frame
        if frame_type == FRAME_TYPE_CONTROL:
            try:
                message = json.loads(payload)
            except json.JSONDecodeError as exc:
                result.mark_error(f"invalid control payload: {exc}")
                break
            msg_type = message.get("type")
            if msg_type == "hello_ack":
                logging.info("[%s] hello_ack status=%s mode=%s",
                             result.client_id,
                             message.get("status"),
                             message.get("mode"))
            elif msg_type == "ready":
                state.ready_payload = message
                state.start_event.set()
                logging.info("[%s] ready peer=%s mode=%s",
                             result.client_id,
                             message.get("peer_id"),
                             message.get("mode"))
            elif msg_type == "summary":
                state.summary_received = True
                result.summary = message
                logging.info("[%s] summary received stats=%s", result.client_id, message)
                break
            elif msg_type == "error":
                err_text = str(message.get("message"))
                logging.error("[%s] server error: %s", result.client_id, err_text)
                result.server_errors.append(err_text)
                result.status = "error"
                break
            elif msg_type == "peer_closed":
                logging.warning("[%s] peer_closed reason=%s", result.client_id, message.get("reason"))
            elif msg_type == "pong":
                continue
            else:
                logging.debug("[%s] ignored control message %s", result.client_id, msg_type)
        elif frame_type == FRAME_TYPE_DATA:
            # Distinguish between peer traffic and server push based on expected counts.
            if result.recv_peer < plan.expect_receive:
                result.recv_peer += 1
            else:
                result.recv_extra += 1
        else:
            result.mark_error(f"unknown frame type {frame_type}")
            break
    state.stop_event.set()


async def sender_task(writer: asyncio.StreamWriter, state: BotState, runtime: RuntimeConfig) -> None:
    plan = state.plan
    result = state.result
    try:
        await asyncio.wait_for(state.start_event.wait(), timeout=runtime.handshake_timeout_ms / 1000.0)
    except asyncio.TimeoutError:
        logging.warning("[%s] handshake ready wait exceeded %d ms (peer not ready)",
                        result.client_id,
                        runtime.handshake_timeout_ms)
        result.mark_error("ready timeout")
        state.stop_event.set()
        try:
            writer.close()
        except Exception:  # pragma: no cover - best effort
            pass
        return

    delay = runtime.send_delay_ms / 1000.0
    for seq in range(plan.send_plan):
        if state.stop_event.is_set():
            break
        payload = build_data_payload(seq, plan.payload_bytes)
        await write_data(writer, payload)
        result.sent += 1
        if delay > 0:
            await asyncio.sleep(delay)

    if plan.shutdown_after_send and not state.stop_event.is_set():
        await write_control(writer, {"type": "done", "sent": result.sent})
        logging.info("[%s] sent done (messages=%d)", result.client_id, result.sent)


async def run_bot(idx: int,
                  plan: BotPlan,
                  transport: TransportConfig,
                  runtime: RuntimeConfig) -> BotResult:
    result = BotResult(client_id=plan.client_id, room=plan.room)
    state = BotState(plan=plan, result=result)
    start = time.perf_counter()
    ssl_ctx = create_ssl_context(transport.tls)

    try:
        connect_coro = asyncio.open_connection(transport.host, transport.port, ssl=ssl_ctx)
        reader, writer = await asyncio.wait_for(connect_coro, timeout=runtime.connect_timeout_ms / 1000.0)
    except Exception as exc:  # pylint: disable=broad-except
        result.mark_error(f"connect failed: {exc}")
        result.duration_sec = time.perf_counter() - start
        return result

    try:
        hello = {
            "type": "hello",
            "version": 1,
            "client_id": plan.client_id,
            "send_plan": plan.send_plan,
            "payload_bytes": plan.payload_bytes,
            "expect_receive": plan.expect_receive,
            "server_push": plan.server_push,
            "shutdown_after_send": plan.shutdown_after_send,
            "wait_timeout_ms": runtime.handshake_timeout_ms,
        }
        if plan.room:
            hello["room"] = plan.room
        mode_value = plan.mode.lower()
        if mode_value not in {"pair", "solo"}:
            mode_value = "pair"
        hello["mode"] = mode_value
        if mode_value == "solo":
            hello["solo"] = True
        await write_control(writer, hello)

        await asyncio.gather(
            asyncio.create_task(reader_task(reader, state)),
            asyncio.create_task(sender_task(writer, state, runtime)),
        )
    finally:
        state.stop_event.set()
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:  # pragma: no cover - best effort
            pass

    result.duration_sec = time.perf_counter() - start
    result.merge_summary()

    if result.sent != plan.send_plan:
        result.mark_error(f"sent {result.sent} != plan {plan.send_plan}")
    if plan.expect_receive >= 0 and result.recv_peer < plan.expect_receive:
        result.mark_error(
            f"recv_peer {result.recv_peer} < expected {plan.expect_receive}"
        )
    if plan.server_push > 0 and result.recv_extra < plan.server_push:
        result.mark_error(
            f"recv_extra {result.recv_extra} < server_push {plan.server_push}"
        )

    return result


async def orchestrate(plans: List[BotPlan],
                      transport: TransportConfig,
                      runtime: RuntimeConfig) -> List[BotResult]:
    sem = asyncio.Semaphore(max(1, runtime.max_concurrency))
    interval = runtime.connect_interval_ms / 1000.0

    async def launch(idx: int, plan: BotPlan) -> BotResult:
        async with sem:
            return await run_bot(idx, plan, transport, runtime)

    tasks: List[asyncio.Task[BotResult]] = []
    for idx, plan in enumerate(plans):
        tasks.append(asyncio.create_task(launch(idx, plan)))
        if interval > 0 and idx < len(plans) - 1:
            await asyncio.sleep(interval)

    return await asyncio.gather(*tasks)


def summarize_results(results: List[BotResult]) -> None:
    total = len(results)
    success = sum(1 for r in results if r.status == "ok" and not r.errors and not r.server_errors)
    logging.info("completed bots: total=%d success=%d failed=%d", total, success, total - success)
    total_sent = sum(r.sent for r in results)
    total_recv = sum(r.recv_peer for r in results)
    total_extra = sum(r.recv_extra for r in results)
    logging.info("aggregate messages: sent=%d received=%d extra=%d", total_sent, total_recv, total_extra)

    for r in results:
        if r.errors or r.server_errors:
            logging.warning("[%s] errors=%s server=%s summary=%s", r.client_id, r.errors, r.server_errors, r.summary)


def apply_overrides(transport: TransportConfig, runtime: RuntimeConfig, args: argparse.Namespace) -> None:
    if args.host:
        transport.host = args.host
    if args.port:
        transport.port = args.port
    if args.max_concurrency:
        runtime.max_concurrency = args.max_concurrency
    if args.log_level:
        runtime.log_level = args.log_level
    if args.tls_flag is not None:
        transport.tls.enabled = args.tls_flag
    if args.ca:
        transport.tls.ca_file = args.ca
    if args.cert:
        transport.tls.cert_file = args.cert
    if args.key:
        transport.tls.key_file = args.key
    if args.no_verify:
        transport.tls.verify = False


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Chat stability test client")
    parser.add_argument("--config", type=Path, help="Path to JSON configuration")
    parser.add_argument("--host", help="Override server host")
    parser.add_argument("--port", type=int, help="Override server port")
    parser.add_argument("--max-concurrency", dest="max_concurrency", type=int, help="Override max concurrency")
    parser.add_argument("--log-level", help="Override log level")
    parser.add_argument("--tls", dest="tls_flag", action="store_const", const=True, help="Force enable TLS")
    parser.add_argument("--no-tls", dest="tls_flag", action="store_const", const=False, help="Force disable TLS")
    parser.add_argument("--ca", help="CA bundle for TLS")
    parser.add_argument("--cert", help="Client certificate for TLS")
    parser.add_argument("--key", help="Client key for TLS")
    parser.add_argument("--no-verify", dest="no_verify", action="store_true", help="Disable TLS validation")
    return parser


async def async_main(args: argparse.Namespace, config_path: Path) -> int:
    config_data = load_json_config(config_path)
    transport = parse_transport(config_data.get("transport", {}))
    runtime = parse_runtime(config_data.get("runtime", {}))
    plans = derive_bot_plans(config_data)

    apply_overrides(transport, runtime, args)
    logging.basicConfig(
        level=getattr(logging, runtime.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    logging.info("using config file %s", config_path)
    logging.info("loaded %d bot plans targeting %s:%d", len(plans), transport.host, transport.port)
    results = await orchestrate(plans, transport, runtime)
    summarize_results(results)
    failures = [r for r in results if r.status != "ok" or r.errors or r.server_errors]
    return 1 if failures else 0


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    try:
        config_path = discover_config(args.config)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    try:
        return asyncio.run(async_main(args, config_path))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # pylint: disable=broad-except
        logging.error("fatal: %s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
