#!/usr/bin/env python3
"""Analyze proxy capture PCAP/PCAPNG files and summarize TCP flows.

This helper wraps ``tshark`` to extract TCP metadata and produces a concise
text report that highlights connections with resets, half-closed sessions, and
per-direction byte counters. Use it together with ``capture_proxy.py`` to
quickly inspect whether any tunnels terminate abnormally.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import locale
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

Endpoint = Tuple[str, int]


@dataclass
class DirectionStats:
    packets: int = 0
    bytes: int = 0
    syn: int = 0
    fin: int = 0
    rst: int = 0


@dataclass
class FlowStats:
    endpoint_a: Endpoint
    endpoint_b: Endpoint
    first_ts: float
    last_ts: float
    dir_a: DirectionStats = field(default_factory=DirectionStats)
    dir_b: DirectionStats = field(default_factory=DirectionStats)

    def duration(self) -> float:
        return max(0.0, self.last_ts - self.first_ts)

    def total_bytes(self) -> int:
        return self.dir_a.bytes + self.dir_b.bytes

    def status(self) -> str:
        if self.dir_a.rst or self.dir_b.rst:
            return "reset"
        fin_a = self.dir_a.fin > 0
        fin_b = self.dir_b.fin > 0
        if fin_a and fin_b:
            return "closed"
        if fin_a or fin_b:
            return "half-closed"
        return "open"

    def add_packet(
        self,
        src: Endpoint,
        dst: Endpoint,
        timestamp: float,
        length: int,
        syn: bool,
        fin: bool,
        rst: bool,
    ) -> None:
        if src == self.endpoint_a and dst == self.endpoint_b:
            target = self.dir_a
        elif src == self.endpoint_b and dst == self.endpoint_a:
            target = self.dir_b
        else:  # pragma: no cover - defensive
            # Unexpected endpoint swap; treat as best-effort by updating totals.
            target = self.dir_a
        target.packets += 1
        target.bytes += length
        if syn:
            target.syn += 1
        if fin:
            target.fin += 1
        if rst:
            target.rst += 1
        if timestamp < self.first_ts:
            self.first_ts = timestamp
        if timestamp > self.last_ts:
            self.last_ts = timestamp


def find_tshark(explicit: Optional[str]) -> Path:
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"tshark not found at {explicit}")
    located = shutil.which("tshark")
    if located:
        return Path(located)
    default_path = Path("C:/Program Files/Wireshark/tshark.exe")
    if default_path.is_file():
        return default_path
    raise FileNotFoundError("tshark executable not found. Install Wireshark or provide --tshark path.")


def build_tshark_cmd(tshark: Path, pcap: Path, display_filter: Optional[str]) -> List[str]:
    cmd = [
        str(tshark),
        "-n",
        "-r",
        str(pcap),
        "-T",
        "fields",
        "-E",
        "separator=,",
        "-E",
        "quote=d",
        "-E",
        "occurrence=f",
        "-E",
        "header=y",
        "-e",
        "frame.time_epoch",
        "-e",
        "ip.src",
        "-e",
        "ipv6.src",
        "-e",
        "tcp.srcport",
        "-e",
        "ip.dst",
        "-e",
        "ipv6.dst",
        "-e",
        "tcp.dstport",
        "-e",
        "tcp.len",
        "-e",
        "tcp.flags.syn",
        "-e",
        "tcp.flags.ack",
        "-e",
        "tcp.flags.fin",
        "-e",
        "tcp.flags.reset",
    ]
    if display_filter:
        cmd.extend(["-Y", display_filter])
    return cmd


def parse_rows(rows: Iterable[List[str]]) -> Dict[Tuple[Endpoint, Endpoint], FlowStats]:
    flows: Dict[Tuple[Endpoint, Endpoint], FlowStats] = {}
    for row in rows:
        if not row or row[0] == "frame.time_epoch":
            continue
        try:
            timestamp = float(row[0])
        except (TypeError, ValueError):  # pragma: no cover - defensive
            continue
        ipv4_src = row[1].strip() if len(row) > 1 and row[1] else ""
        ipv6_src = row[2].strip() if len(row) > 2 and row[2] else ""
        src_ip = ipv4_src or ipv6_src
        src_port_raw = row[3] if len(row) > 3 else ""
        ipv4_dst = row[4].strip() if len(row) > 4 and row[4] else ""
        ipv6_dst = row[5].strip() if len(row) > 5 and row[5] else ""
        dst_ip = ipv4_dst or ipv6_dst
        dst_port_raw = row[6] if len(row) > 6 else ""
        if not src_ip or not dst_ip or not src_port_raw or not dst_port_raw:
            continue
        try:
            src_port = int(src_port_raw)
            dst_port = int(dst_port_raw)
        except ValueError:
            continue
        length_raw = row[7] if len(row) > 7 else "0"
        try:
            payload_len = int(length_raw) if length_raw else 0
        except ValueError:
            payload_len = 0
        syn_flag = row[8] == "1" if len(row) > 8 else False
        fin_flag = row[10] == "1" if len(row) > 10 else False
        rst_flag = row[11] == "1" if len(row) > 11 else False

        src = (src_ip, src_port)
        dst = (dst_ip, dst_port)
        key = tuple(sorted((src, dst)))
        flow = flows.get(key)
        if flow is None:
            flow = FlowStats(endpoint_a=src, endpoint_b=dst, first_ts=timestamp, last_ts=timestamp)
            flows[key] = flow
        flow.add_packet(src, dst, timestamp, payload_len, syn_flag, fin_flag, rst_flag)
    return flows


def generate_report(flows: Dict[Tuple[Endpoint, Endpoint], FlowStats]) -> str:
    lines: List[str] = []
    lines.append("=== Capture Analysis ===")
    total_flows = len(flows)
    reset_flows = sum(1 for flow in flows.values() if flow.dir_a.rst or flow.dir_b.rst)
    half_closed = sum(1 for flow in flows.values() if flow.status() == "half-closed")
    closed = sum(1 for flow in flows.values() if flow.status() == "closed")
    open_flows = total_flows - reset_flows - half_closed - closed
    lines.append(f"Total TCP flows: {total_flows}")
    lines.append(f"  reset: {reset_flows}")
    lines.append(f"  closed (FIN both sides): {closed}")
    lines.append(f"  half-closed (single FIN): {half_closed}")
    lines.append(f"  open/no FIN: {open_flows}")

    def fmt_endpoint(ep: Endpoint) -> str:
        return f"{ep[0]}:{ep[1]}"

    if reset_flows:
        lines.append("\nConnections with RST events (sorted by newest):")
        reset_items = sorted(
            (flow for flow in flows.values() if flow.dir_a.rst or flow.dir_b.rst),
            key=lambda f: f.last_ts,
            reverse=True,
        )
        for flow in reset_items:
            status = flow.status()
            duration = flow.duration()
            lines.append(
                "  - "
                f"{fmt_endpoint(flow.endpoint_a)} <-> {fmt_endpoint(flow.endpoint_b)} | "
                f"status={status} | duration={duration:.3f}s | "
                f"A->B bytes={flow.dir_a.bytes} packets={flow.dir_a.packets} | "
                f"B->A bytes={flow.dir_b.bytes} packets={flow.dir_b.packets} | "
                f"RST A={flow.dir_a.rst} B={flow.dir_b.rst} | FIN A={flow.dir_a.fin} B={flow.dir_b.fin}"
            )

    richest = sorted(flows.values(), key=lambda f: f.total_bytes(), reverse=True)[:10]
    if richest:
        lines.append("\nTop flows by transferred bytes:")
        for flow in richest:
            status = flow.status()
            duration = flow.duration()
            lines.append(
                "  - "
                f"{fmt_endpoint(flow.endpoint_a)} <-> {fmt_endpoint(flow.endpoint_b)} | "
                f"status={status} | duration={duration:.3f}s | total_bytes={flow.total_bytes()} | "
                f"A->B bytes={flow.dir_a.bytes} ({flow.dir_a.packets} pkts, SYN={flow.dir_a.syn}, FIN={flow.dir_a.fin}, RST={flow.dir_a.rst}) | "
                f"B->A bytes={flow.dir_b.bytes} ({flow.dir_b.packets} pkts, SYN={flow.dir_b.syn}, FIN={flow.dir_b.fin}, RST={flow.dir_b.rst})"
            )

    return "\n".join(lines)


def analyze_capture(
    pcap_path: Path,
    tshark_path: Path,
    display_filter: Optional[str],
    output_path: Optional[Path],
) -> Path:
    cmd = build_tshark_cmd(tshark_path, pcap_path, display_filter)
    encoding = locale.getpreferredencoding(False) or "utf-8"
    with subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding=encoding,
        errors="replace",
    ) as proc:
        if proc.stdout is None:
            raise RuntimeError("failed to read tshark output")
        reader = csv.reader(proc.stdout)
        flows = parse_rows(reader)
        stderr_output = proc.stderr.read() if proc.stderr else ""
        return_code = proc.wait()
    if return_code != 0:
        raise RuntimeError(f"tshark exited with {return_code}: {stderr_output.strip()}")

    report = generate_report(flows)
    if output_path is None:
        timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = pcap_path.with_suffix("").parent / f"capture_analysis_{timestamp}.log"
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(report, encoding="utf-8")
    return output_path


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze proxy captures using tshark")
    parser.add_argument("pcap", type=Path, help="PCAP/PCAPNG file to analyze")
    parser.add_argument("--tshark", dest="tshark_path", help="Explicit tshark executable path")
    parser.add_argument(
        "--display-filter",
        dest="display_filter",
        default="tcp",
        help="Wireshark display filter applied during parsing (default: tcp)",
    )
    parser.add_argument(
        "--output",
        dest="output_path",
        type=Path,
        help="Optional output path for the generated report (default: logs/capture_analysis_*.log)",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        pcap_path = args.pcap.expanduser().resolve()
        if not pcap_path.exists():
            raise FileNotFoundError(f"capture file not found: {pcap_path}")
        tshark_path = find_tshark(args.tshark_path)
        output_path = args.output_path.expanduser().resolve() if args.output_path else None
        report_path = analyze_capture(pcap_path, tshark_path, args.display_filter, output_path)
        print(f"Analysis written to {report_path}")
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
