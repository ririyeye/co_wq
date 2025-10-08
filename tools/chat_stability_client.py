#!/usr/bin/env python3
"""Async chat stability load client for the co_wq test server."""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import logging
import math
import os
import random
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field, replace
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
    transport: TransportConfig = field(default_factory=TransportConfig)
    room: Optional[str] = None
    mode: str = "pair"
    workload: Optional[str] = None


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
    workload: Optional[str] = None
    host: str = ""
    port: int = 0
    tls_enabled: bool = False
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


def clone_tls_options(tls: TLSOptions) -> TLSOptions:
    return replace(
        tls,
        enabled=bool(tls.enabled),
        verify=bool(tls.verify),
        ca_file=tls.ca_file,
        cert_file=tls.cert_file,
        key_file=tls.key_file,
    )


def clone_transport(config: TransportConfig) -> TransportConfig:
    return TransportConfig(
        host=config.host,
        port=config.port,
        tls=clone_tls_options(config.tls),
    )


def merge_range_spec(spec: Any, fallback: Any) -> Any:
    if spec is None:
        return fallback
    if isinstance(spec, dict) and isinstance(fallback, dict):
        merged = dict(fallback)
        merged.update(spec)
        return merged
    return spec


def pick_from_range_spec(spec: Any, base_value: int = 0) -> int:
    if isinstance(spec, dict):
        if "value" in spec:
            return int(spec["value"])
        min_val = spec.get("min")
        if min_val is None:
            min_val = spec.get("min_bytes")
        max_val = spec.get("max")
        if max_val is None:
            max_val = spec.get("max_bytes")
        if min_val is None and max_val is None:
            range_spec = spec.get("range")
            if isinstance(range_spec, (list, tuple)) and len(range_spec) >= 2:
                min_val, max_val = range_spec[0], range_spec[1]
        if min_val is None:
            min_val = base_value
        if max_val is None:
            max_val = min_val
        min_int = int(min_val)
        max_int = int(max_val)
        if min_int > max_int:
            min_int, max_int = max_int, min_int
        return random.randint(min_int, max_int)
    if isinstance(spec, (list, tuple)):
        if not spec:
            return base_value
        if len(spec) == 1:
            return int(spec[0])
        min_int = int(spec[0])
        max_int = int(spec[-1])
        if min_int > max_int:
            min_int, max_int = max_int, min_int
        return random.randint(min_int, max_int)
    if isinstance(spec, str):
        stripped = spec.strip()
        if stripped in {"send_plan", "payload_bytes", "expect_receive"}:
            return base_value
        if "~" in stripped:
            parts = stripped.split("~", 1)
            try:
                min_int = int(parts[0])
                max_int = int(parts[1])
                if min_int > max_int:
                    min_int, max_int = max_int, min_int
                return random.randint(min_int, max_int)
            except ValueError:
                pass
        return int(stripped)
    return int(spec)


def choose_int_value(spec: Any, fallback: Any = None, base_value: int = 0) -> int:
    effective = merge_range_spec(spec, fallback)
    if effective is None:
        return base_value
    return pick_from_range_spec(effective, base_value=base_value)


def choose_expect_value(spec: Any, fallback: Any, send_plan: int) -> int:
    if spec is None and fallback is None:
        return send_plan
    if isinstance(spec, str) and spec.strip().lower() == "send_plan":
        return send_plan
    return choose_int_value(spec, fallback, base_value=send_plan)


def merge_transport(base: TransportConfig, overrides: Optional[Dict[str, Any]]) -> TransportConfig:
    cfg = clone_transport(base)
    if not overrides:
        return cfg

    if "host" in overrides:
        cfg.host = str(overrides["host"])
    if "port" in overrides:
        cfg.port = int(overrides["port"])

    mode = overrides.get("mode")
    if isinstance(mode, str):
        lower = mode.lower()
        if lower in {"tls", "ssl"}:
            cfg.tls.enabled = True
        elif lower in {"tcp", "plain"}:
            cfg.tls.enabled = False

    tls_override = overrides.get("tls")
    if isinstance(tls_override, dict):
        if "enabled" in tls_override:
            cfg.tls.enabled = bool(tls_override["enabled"])
        if "verify" in tls_override:
            cfg.tls.verify = bool(tls_override["verify"])
        if "ca_file" in tls_override:
            cfg.tls.ca_file = tls_override["ca_file"]
        if "cert" in tls_override or "cert_file" in tls_override:
            cfg.tls.cert_file = tls_override.get("cert", tls_override.get("cert_file"))
        if "key" in tls_override or "key_file" in tls_override:
            cfg.tls.key_file = tls_override.get("key", tls_override.get("key_file"))

    # direct TLS shorthands on the transport block
    if "tls_enabled" in overrides:
        cfg.tls.enabled = bool(overrides["tls_enabled"])
    if "tls_verify" in overrides:
        cfg.tls.verify = bool(overrides["tls_verify"])
    if "ca" in overrides:
        cfg.tls.ca_file = overrides["ca"]
    if "cert" in overrides:
        cfg.tls.cert_file = overrides["cert"]
    if "key" in overrides:
        cfg.tls.key_file = overrides["key"]

    return cfg


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


def derive_bot_plans(config: Dict[str, Any], base_transport: TransportConfig) -> List[BotPlan]:
    defaults = config.get("defaults", {})
    send_plan_default_spec = defaults.get("send_plan", defaults.get("cnt"))
    payload_default_spec = defaults.get("payload_bytes")
    expect_default_spec = defaults.get("expect_receive")
    server_push_default_spec = defaults.get("server_push", 0)
    shutdown_def = bool(defaults.get("shutdown_after_send", True))
    mode_def = str(defaults.get("mode", "pair")).lower()
    if defaults.get("solo") is True:
        mode_def = "solo"

    plans: List[BotPlan] = []
    max_payload_body = MAX_FRAME_BYTES - 8

    for entry in config.get("workloads", []):
        workload_name = entry.get("name") or entry.get("id") or entry.get("label")
        mode = str(entry.get("mode", mode_def)).lower()
        if entry.get("solo") is True:
            mode = "solo"
        if entry.get("pair") is True:
            mode = "pair"
        if mode not in {"pair", "solo"}:
            mode = "pair"

        transport_template = merge_transport(base_transport, entry.get("transport"))

        send_plan_spec = entry.get("send_plan", entry.get("cnt", send_plan_default_spec))
        payload_spec = entry.get("payload_bytes")
        if payload_spec is None:
            payload_spec = entry.get("payload_bytes_range", payload_default_spec)
        expect_spec = entry.get("expect_receive", expect_default_spec)
        server_push_spec = entry.get("server_push", server_push_default_spec)
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

                    send_plan_val = max(0, choose_int_value(send_plan_spec, send_plan_default_spec, base_value=0))
                    payload_val = max(0, choose_int_value(payload_spec, payload_default_spec, base_value=0))
                    if payload_val > 0 and payload_val % 4 != 0:
                        payload_val += 4 - (payload_val % 4)
                    if payload_val > max_payload_body:
                        payload_val = max_payload_body
                    expect_val = max(0, choose_expect_value(expect_spec, expect_default_spec, send_plan_val))
                    server_push_val = max(0, choose_int_value(server_push_spec, server_push_default_spec, base_value=0))

                    plans.append(
                        BotPlan(
                            client_id=client_id,
                            send_plan=send_plan_val,
                            payload_bytes=payload_val,
                            expect_receive=expect_val,
                            server_push=server_push_val,
                            shutdown_after_send=shutdown,
                            transport=clone_transport(transport_template),
                            room=room_name,
                            mode=mode,
                            workload=workload_name,
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

                send_plan_val = max(0, choose_int_value(send_plan_spec, send_plan_default_spec, base_value=0))
                payload_val = max(0, choose_int_value(payload_spec, payload_default_spec, base_value=0))
                if payload_val > 0 and payload_val % 4 != 0:
                    payload_val += 4 - (payload_val % 4)
                if payload_val > max_payload_body:
                    payload_val = max_payload_body
                expect_val = max(0, choose_expect_value(expect_spec, expect_default_spec, send_plan_val))
                server_push_val = max(0, choose_int_value(server_push_spec, server_push_default_spec, base_value=0))

                plans.append(
                    BotPlan(
                        client_id=client_id,
                        send_plan=send_plan_val,
                        payload_bytes=payload_val,
                        expect_receive=expect_val,
                        server_push=server_push_val,
                        shutdown_after_send=shutdown,
                        transport=clone_transport(transport_template),
                        room=room_name if room_name else None,
                        mode=mode,
                        workload=workload_name,
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

        transport_template = merge_transport(base_transport, entry.get("transport"))

        send_plan_spec = entry.get("send_plan", entry.get("cnt", send_plan_default_spec))
        payload_spec = entry.get("payload_bytes")
        if payload_spec is None:
            payload_spec = entry.get("payload_bytes_range", payload_default_spec)
        expect_spec = entry.get("expect_receive", expect_default_spec)
        server_push_spec = entry.get("server_push", server_push_default_spec)
        shutdown = bool(entry.get("shutdown_after_send", shutdown_def))

        send_plan_val = max(0, choose_int_value(send_plan_spec, send_plan_default_spec, base_value=0))
        payload_val = max(0, choose_int_value(payload_spec, payload_default_spec, base_value=0))
        if payload_val > 0 and payload_val % 4 != 0:
            payload_val += 4 - (payload_val % 4)
        if payload_val > max_payload_body:
            payload_val = max_payload_body
        expect_val = max(0, choose_expect_value(expect_spec, expect_default_spec, send_plan_val))
        server_push_val = max(0, choose_int_value(server_push_spec, server_push_default_spec, base_value=0))

        plans.append(
            BotPlan(
                client_id=str(client_id),
                send_plan=send_plan_val,
                payload_bytes=payload_val,
                expect_receive=expect_val,
                server_push=server_push_val,
                shutdown_after_send=shutdown,
                transport=transport_template,
                room=None if room_value in (None, "") else str(room_value),
                mode=mode,
                workload=entry.get("name") or entry.get("label"),
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


def random_base64_bytes(length: int) -> bytes:
    if length <= 0:
        return b""
    raw_len = max(1, math.ceil(length * 3 / 4))
    encoded = base64.b64encode(os.urandom(raw_len))
    if len(encoded) >= length:
        return encoded[:length]
    parts = [encoded]
    remaining = length - len(encoded)
    while remaining > 0:
        chunk_raw = max(1, math.ceil(remaining * 3 / 4))
        chunk = base64.b64encode(os.urandom(chunk_raw))
        if len(chunk) >= remaining:
            parts.append(chunk[:remaining])
            break
        parts.append(chunk)
        remaining -= len(chunk)
    return b"".join(parts)


def build_data_payload(seq: int, payload_bytes: int) -> bytes:
    body = random_base64_bytes(payload_bytes)
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
                  runtime: RuntimeConfig) -> BotResult:
    transport = plan.transport
    result = BotResult(
        client_id=plan.client_id,
        room=plan.room,
        workload=plan.workload,
        host=transport.host,
        port=transport.port,
        tls_enabled=transport.tls.enabled,
    )
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
                      runtime: RuntimeConfig) -> List[BotResult]:
    sem = asyncio.Semaphore(max(1, runtime.max_concurrency))
    interval = runtime.connect_interval_ms / 1000.0

    async def launch(idx: int, plan: BotPlan) -> BotResult:
        async with sem:
            return await run_bot(idx, plan, runtime)

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
            label = r.client_id if not r.workload else f"{r.client_id}@{r.workload}"
            endpoint = f"{r.host}:{r.port}" if r.host else "<unknown>"
            proto = "tls" if r.tls_enabled else "tcp"
            logging.warning(
                "[%s] target=%s(%s) errors=%s server=%s summary=%s",
                label,
                endpoint,
                proto,
                r.errors,
                r.server_errors,
                r.summary,
            )


def apply_overrides(plans: List[BotPlan], runtime: RuntimeConfig, args: argparse.Namespace) -> None:
    if args.host:
        for plan in plans:
            plan.transport.host = args.host
    if args.port:
        for plan in plans:
            plan.transport.port = args.port
    if args.max_concurrency:
        runtime.max_concurrency = args.max_concurrency
    if args.log_level:
        runtime.log_level = args.log_level
    if args.tls_flag is not None:
        for plan in plans:
            plan.transport.tls.enabled = args.tls_flag
    if args.ca:
        for plan in plans:
            plan.transport.tls.ca_file = args.ca
    if args.cert:
        for plan in plans:
            plan.transport.tls.cert_file = args.cert
    if args.key:
        for plan in plans:
            plan.transport.tls.key_file = args.key
    if args.no_verify:
        for plan in plans:
            plan.transport.tls.verify = False


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
    base_transport = parse_transport(config_data.get("transport", {}))
    runtime = parse_runtime(config_data.get("runtime", {}))
    plans = derive_bot_plans(config_data, base_transport)

    apply_overrides(plans, runtime, args)
    logging.basicConfig(
        level=getattr(logging, runtime.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    logging.info("using config file %s", config_path)
    targets = sorted({
        (plan.transport.host,
         plan.transport.port,
         "tls" if plan.transport.tls.enabled else "tcp")
        for plan in plans
    })
    target_desc = ", ".join(f"{host}:{port}({proto})" for host, port, proto in targets)
    logging.info(
        "loaded %d bot plans across %d targets: %s",
        len(plans),
        len(targets),
        target_desc or "<none>",
    )
    results = await orchestrate(plans, runtime)
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
