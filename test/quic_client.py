import argparse
import asyncio
import datetime
import os
import random
import ssl
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

try:
    from aioquic.asyncio import connect
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.events import (  # type: ignore[attr-defined]
        ConnectionTerminated,
        HandshakeCompleted,
        StreamDataReceived,
    )
except ImportError as exc:  # pragma: no cover
    print(
        "error: aioquic is required (install via 'pip install aioquic').",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc


def timestamp() -> str:
    return datetime.datetime.now().strftime("[%Y-%m-%d %H:%M:%S.%f]")


@dataclass(frozen=True)
class ScheduledPayload:
    delay_ms: int
    data: bytes

    @property
    def delay_seconds(self) -> float:
        return self.delay_ms / 1000.0


@dataclass(frozen=True)
class ClientResult:
    client_id: int
    success: bool
    reason: Optional[str] = None
    status: Optional[Dict[str, object]] = None


class EchoClient(QuicConnectionProtocol):
    """QUIC echo client with verbose logging, random payloads, and validation."""

    def __init__(
        self,
        *args,
        client_id: int,
        schedule: List[ScheduledPayload],
        finished: asyncio.Future,
        **kwargs,
    ) -> None:
        super().__init__(*args, **kwargs)
        self._client_id = client_id
        self._schedule = schedule
        self._finished: asyncio.Future = finished
        self._stream_id: Optional[int] = None
        self._buffer = bytearray()
        self._expected_total = sum(len(item.data) for item in self._schedule)
        self._prefix = f"[client-{self._client_id}]"
        self._send_task: Optional[asyncio.Task] = None
        self._last_progress = time.monotonic()
        self._chunks_sent = 0
        self._bytes_sent = 0
        self._bytes_received = 0
        self._receive_events = 0
        self._handshake_completed = False
        self._last_error: Optional[str] = None

    def quic_event_received(self, event) -> None:  # type: ignore[override]
        if isinstance(event, HandshakeCompleted):
            self._record_progress()
            peername = (
                self._transport.get_extra_info("peername")
                if self._transport
                else None
            )
            print(
                f"{timestamp()} {self._prefix} handshake completed alpn={event.alpn_protocol} peer={peername}"
            )

            self._stream_id = self._quic.get_next_available_stream_id(
                is_unidirectional=False
            )
            self._handshake_completed = True
            print(
                f"{timestamp()} {self._prefix} opened bidirectional stream {self._stream_id}, scheduled messages={len(self._schedule)} total bytes={self._expected_total}"
            )

            if self._schedule:
                loop = asyncio.get_running_loop()
                self._send_task = loop.create_task(
                    self._send_scheduled_messages()
                )
        elif isinstance(event, StreamDataReceived):
            self._record_progress()
            if event.stream_id != self._stream_id:
                return
            self._buffer.extend(event.data)
            self._receive_events += 1
            self._bytes_received += len(event.data)
            print(
                f"{timestamp()} {self._prefix} <- chunk bytes={len(event.data)} end={event.end_stream} total={len(self._buffer)}/{self._expected_total}"
            )
            if event.end_stream and not self._finished.done():
                if len(self._buffer) != self._expected_total:
                    error_msg = f"echo size mismatch expected={self._expected_total} got={len(self._buffer)}"
                    print(
                        f"{timestamp()} {self._prefix} validation failed: {error_msg}"
                    )
                    self._record_error(error_msg)
                    self._finished.set_exception(ValueError(error_msg))
                else:
                    print(
                        f"{timestamp()} {self._prefix} validation succeeded: received all {self._expected_total} bytes"
                    )
                    self._last_error = None
                    self._finished.set_result(bytes(self._buffer))
        elif isinstance(event, ConnectionTerminated):
            self._record_progress()
            print(
                f"{timestamp()} {self._prefix} connection terminated error_code={event.error_code} frame_type={event.frame_type}"
            )
            if self._send_task and not self._send_task.done():
                self._send_task.cancel()
            if not self._finished.done():
                reason = (
                    f"connection terminated (error_code={event.error_code}, frame_type={event.frame_type})"
                )
                self._record_error(reason)
                self._finished.set_exception(
                    RuntimeError(
                        f"connection terminated (error_code={event.error_code}, frame_type={event.frame_type})"
                    )
                )

    async def _send_scheduled_messages(self) -> None:
        try:
            for index, item in enumerate(self._schedule, start=1):
                if item.delay_ms > 0:
                    await asyncio.sleep(item.delay_seconds)

                end_stream = index == len(self._schedule)
                preview = item.data[:16].hex()
                print(
                    f"{timestamp()} {self._prefix} -> chunk#{index} bytes={len(item.data)} delay={item.delay_ms}ms end={end_stream} preview={preview}"
                )
                self._quic.send_stream_data(
                    self._stream_id, item.data, end_stream=end_stream
                )
                self.transmit()
                self._chunks_sent = index
                self._bytes_sent += len(item.data)
                self._record_progress()
        except asyncio.CancelledError:
            pass

    def connection_lost(self, exc: Optional[BaseException]) -> None:
        if self._send_task and not self._send_task.done():
            self._send_task.cancel()
        super().connection_lost(exc)

    @property
    def last_progress(self) -> float:
        return self._last_progress

    def _record_progress(self) -> None:
        self._last_progress = time.monotonic()

    def _record_error(self, message: str) -> None:
        self._last_error = message

    def status_snapshot(self) -> dict:
        return {
            "handshake_completed": self._handshake_completed,
            "stream_id": self._stream_id,
            "scheduled_chunks": len(self._schedule),
            "chunks_sent": self._chunks_sent,
            "bytes_scheduled": self._expected_total,
            "bytes_sent": self._bytes_sent,
            "bytes_received": self._bytes_received,
            "receive_events": self._receive_events,
            "buffer_size": len(self._buffer),
            "finished_done": self._finished.done(),
            "send_task_done": self._send_task.done() if self._send_task else True,
            "last_error": self._last_error,
            "last_progress_age": time.monotonic() - self._last_progress,
        }


def build_random_schedule(args: argparse.Namespace) -> List[ScheduledPayload]:
    message_count = random.randint(1, 10)
    schedule: List[ScheduledPayload] = []
    for _ in range(message_count):
        length = random.randint(1, 10_240)
        delay_ms = random.randint(0, 1000)
        data = os.urandom(length)
        schedule.append(ScheduledPayload(delay_ms=delay_ms, data=data))
    return schedule


async def run_single_client(client_id: int, args: argparse.Namespace) -> ClientResult:
    loop = asyncio.get_running_loop()
    finished: asyncio.Future = loop.create_future()

    schedule = build_random_schedule(args)
    total_expected = sum(len(item.data) for item in schedule)
    prefix = f"[client-{client_id}]"

    print(
        f"{timestamp()} {prefix} connecting to {args.host}:{args.port} alpn={args.alpn} messages={len(schedule)} expected echo bytes={total_expected}"
    )

    configuration = QuicConfiguration(
        is_client=True, alpn_protocols=[args.alpn]
    )
    configuration.server_name = args.sni or args.host

    if args.ca:
        configuration.load_verify_locations(args.ca)
    elif args.insecure:
        configuration.verify_mode = ssl.CERT_NONE
        configuration.check_hostname = False

    def protocol_factory(*factory_args, **factory_kwargs):
        return EchoClient(
            *factory_args,
            client_id=client_id,
            schedule=schedule,
            finished=finished,
            **factory_kwargs,
        )

    protocol: Optional[EchoClient] = None
    try:
        async with connect(
            args.host,
            args.port,
            configuration=configuration,
            create_protocol=protocol_factory,
            wait_connected=True,
        ) as connected_protocol:
            assert isinstance(connected_protocol, EchoClient)
            protocol = connected_protocol

            try:
                data = await wait_until_finished(
                    connected_protocol, finished, args.timeout
                )
            except asyncio.CancelledError:
                status = log_failure(prefix, protocol, "wait cancelled")
                protocol.close()
                return ClientResult(
                    client_id=client_id,
                    success=False,
                    reason="wait cancelled",
                    status=status,
                )
            except asyncio.TimeoutError:
                status = log_failure(
                    prefix, protocol, "timeout waiting for response"
                )
                protocol.close()
                return ClientResult(
                    client_id=client_id,
                    success=False,
                    reason="timeout waiting for response",
                    status=status,
                )
            except Exception as exc:  # pragma: no cover
                reason = f"error during exchange: {exc}"
                status = log_failure(prefix, protocol, reason)
                protocol.close()
                return ClientResult(
                    client_id=client_id,
                    success=False,
                    reason=reason,
                    status=status,
                )

            if data:
                joined = b"".join(item.data for item in schedule)
                if data != joined:
                    print(
                        f"{timestamp()} {prefix} warning: echo payload differs from what was sent",
                        file=sys.stderr,
                    )
                else:
                    print(
                        f"{timestamp()} {prefix} echo verified: received {len(data)} bytes matching the sent payloads"
                    )

                offset = 0
                for index, item in enumerate(schedule, start=1):
                    segment = data[offset: offset + len(item.data)]
                    print(
                        f"{timestamp()} {prefix} echoed chunk#{index} bytes={len(segment)} preview={segment[:16].hex()}"
                    )
                    offset += len(item.data)
    except Exception as exc:  # pragma: no cover
        reason = f"failed to establish connection: {exc}"
        status = log_failure(prefix, protocol, reason)
        return ClientResult(
            client_id=client_id,
            success=False,
            reason=reason,
            status=status,
        )

    status = protocol.status_snapshot() if protocol else None
    return ClientResult(
        client_id=client_id,
        success=True,
        reason=None,
        status=status,
    )


def log_failure(
    prefix: str, protocol: Optional[EchoClient], reason: str
) -> Optional[Dict[str, object]]:
    print(f"{timestamp()} {prefix} failure reason={reason}", file=sys.stderr)
    if not protocol:
        return None
    status = protocol.status_snapshot()
    print(
        (
            f"{timestamp()} {prefix} status "
            f"handshake={status['handshake_completed']} stream_id={status['stream_id']} "
            f"chunks_sent={status['chunks_sent']}/{status['scheduled_chunks']} "
            f"bytes_sent={status['bytes_sent']}/{status['bytes_scheduled']} "
            f"bytes_received={status['bytes_received']} recv_events={status['receive_events']} "
            f"buffer_bytes={status['buffer_size']} send_task_done={status['send_task_done']} "
            f"finished={status['finished_done']} last_error={status['last_error']} "
            f"last_progress_idle={status['last_progress_age']:.3f}s"
        ),
        file=sys.stderr,
    )
    return status


async def run_parallel_clients(args: argparse.Namespace) -> None:
    tasks = [
        run_single_client(client_id=index + 1, args=args)
        for index in range(args.parallel)
    ]

    results = await asyncio.gather(*tasks)
    success = sum(1 for result in results if result.success)
    total = len(results)

    print(
        f"{timestamp()} [main] parallel run finished success={success}/{total}"
    )

    failures = [result for result in results if not result.success]
    if failures:
        print(
            f"{timestamp()} [main] failures={len(failures)}", file=sys.stderr
        )
        for result in failures:
            reason = result.reason or "unknown"
            print(
                f"{timestamp()} [main] failed client={result.client_id} reason={reason}",
                file=sys.stderr,
            )
            if result.status:
                status = result.status
                print(
                    (
                        f"{timestamp()} [main] status"
                        f" chunks_sent={status.get('chunks_sent')}"
                        f"/{status.get('scheduled_chunks')}"
                        f" bytes_sent={status.get('bytes_sent')}"
                        f"/{status.get('bytes_scheduled')}"
                        f" bytes_received={status.get('bytes_received')}"
                        f" recv_events={status.get('receive_events')}"
                        f" finished={status.get('finished_done')}"
                        f" last_error={status.get('last_error')}"
                    ),
                    file=sys.stderr,
                )


async def wait_until_finished(
    protocol: EchoClient, finished: asyncio.Future, timeout: float
) -> bytes:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    last_progress = protocol.last_progress

    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise asyncio.TimeoutError

        try:
            return await asyncio.wait_for(
                asyncio.shield(finished), timeout=remaining
            )
        except asyncio.TimeoutError:
            new_progress = protocol.last_progress
            if new_progress > last_progress:
                last_progress = new_progress
                deadline = loop.time() + timeout
                continue
            raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Simple QUIC echo client for co_quic_server testing.",
    )
    parser.add_argument(
        "host",
        nargs="?",
        default="127.0.0.1",
        help="Server hostname or IP (default: 127.0.0.1)",
    )
    parser.add_argument(
        "port",
        nargs="?",
        type=int,
        default=6121,
        help="Server port (default: 6121)",
    )
    parser.add_argument(
        "--message",
        default="ping",
        help="Base payload used to build three variable-length chunks.",
    )
    parser.add_argument(
        "--alpn",
        default="http/1.0",
        help="ALPN to advertise during the QUIC handshake.",
    )
    parser.add_argument(
        "--ca",
        help="Custom CA bundle for certificate verification.",
    )
    parser.add_argument(
        "--sni",
        help="Override SNI/hostname used during verification.",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        default=True,
        help="Disable certificate validation (default; use --secure to re-enable).",
    )
    parser.add_argument(
        "--secure",
        dest="insecure",
        action="store_false",
        help="Enable certificate validation.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Seconds of inactivity to allow before failing (per client).",
    )
    parser.add_argument(
        "--parallel",
        type=int,
        default=10,
        help="Number of concurrent connections to launch (default: 10).",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    try:
        asyncio.run(run_parallel_clients(args))
    except KeyboardInterrupt:  # pragma: no cover
        pass


if __name__ == "__main__":
    main()
