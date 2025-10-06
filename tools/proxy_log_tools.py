#!/usr/bin/env python3
"""Utility helpers for analyzing co_wq proxy logs.

The script digs through the structured logging emitted by `test/http_proxy.cpp`
so we can quickly answer questions that previously required one-off REPL
snippets, such as:

* How many relay summaries ended with each termination reason?
* Which client connections experienced recv errors and what were the exact
  Winsock codes?

Example usage::

    python tools/proxy_log_tools.py reason-counts
    python tools/proxy_log_tools.py reason-counts --include-debug
    python tools/proxy_log_tools.py list-errors --target baidu

Both subcommands operate on ``logs/proxy.log`` by default; pass
``--log <path>`` to inspect another file.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence

SUMMARY_RE = re.compile(
    r"\[client \[::ffff:(?P<ip>[\d\.]+)]:(?P<port>\d+)] CONNECT (?P<context>[^ ]+) "
    r"summary: (?P<metrics>.*)"
)
CONNECT_TARGET_RE = re.compile(
    r"\[client \[::ffff:(?P<ip>[\d\.]+)]:(?P<port>\d+)] received request CONNECT (?P<target>[^ ]+)"
)


@dataclass
class SummaryEntry:
    """Parsed information for a single relay summary line."""

    ip: str
    port: str
    context: str
    metrics: str
    reason: str
    raw_line: str


def _read_lines(log_path: pathlib.Path) -> List[str]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    return text.splitlines()


def _extract_targets(lines: Iterable[str]) -> Dict[str, str]:
    targets: Dict[str, str] = {}
    for line in lines:
        match = CONNECT_TARGET_RE.search(line)
        if match:
            targets[match.group("port")] = match.group("target")
    return targets


def parse_summaries(lines: Sequence[str], *, include_debug: bool = False) -> List[SummaryEntry]:
    summaries: List[SummaryEntry] = []
    for line in lines:
        if "summary:" not in line:
            continue
        if not include_debug and "] [debug]" in line:
            continue
        if " reason=" not in line:
            continue
        prefix, reason = line.split(" reason=", 1)
        match = SUMMARY_RE.search(prefix)
        if not match:
            continue
        summaries.append(
            SummaryEntry(
                ip=match.group("ip"),
                port=match.group("port"),
                context=match.group("context"),
                metrics=match.group("metrics").strip(),
                reason=reason.strip(),
                raw_line=line,
            )
        )
    return summaries


def command_reason_counts(args: argparse.Namespace) -> None:
    log_path = pathlib.Path(args.log)
    lines = _read_lines(log_path)
    summaries = parse_summaries(lines, include_debug=args.include_debug)
    counter = collections.Counter(entry.reason for entry in summaries)

    print(f"total summaries: {len(summaries)}")
    for reason, count in counter.most_common():
        print(f"{reason}: {count}")


def command_list_errors(args: argparse.Namespace) -> None:
    log_path = pathlib.Path(args.log)
    lines = _read_lines(log_path)
    targets = _extract_targets(lines)
    summaries = parse_summaries(lines, include_debug=args.include_debug)

    grouped: Dict[str, List[SummaryEntry]] = collections.defaultdict(list)
    for entry in summaries:
        grouped[entry.port].append(entry)

    error_groups = []
    for port, entries in grouped.items():
        if not any("recv_error" in e.reason for e in entries):
            continue
        target = targets.get(port)
        if args.target and (not target or args.target.lower() not in target.lower()):
            continue
        error_groups.append((port, target, entries))

    error_groups.sort(key=lambda item: int(item[0]))
    if args.limit is not None:
        error_groups = error_groups[: args.limit]

    for port, target, entries in error_groups:
        header = f"port {port} target {target or '?'}"
        print(header)
        for entry in entries:
            print(f"  {entry.context}: {entry.reason}")
        print()

    total_connections = len(grouped)
    print(
        f"connections with errors: {len(error_groups)} / {total_connections}"
        if total_connections
        else "connections with errors: 0"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze structured proxy logs.")
    parser.add_argument(
        "--log",
        default="logs/proxy.log",
        help="Path to the log file (default: logs/proxy.log)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    parser_counts = subparsers.add_parser(
        "reason-counts", help="Count termination reasons across all summaries."
    )
    parser_counts.add_argument(
        "--include-debug",
        action="store_true",
        help="Include duplicate [debug] summaries (default: skip).",
    )
    parser_counts.set_defaults(func=command_reason_counts)

    parser_errors = subparsers.add_parser(
        "list-errors",
        help="List connections whose summaries contain recv_error reasons.",
    )
    parser_errors.add_argument(
        "--include-debug",
        action="store_true",
        help="Include duplicate [debug] summaries (default: skip).",
    )
    parser_errors.add_argument(
        "--target",
        help="Optional substring filter for CONNECT targets (case-insensitive).",
    )
    parser_errors.add_argument(
        "--limit",
        type=int,
        help="Limit the number of connections shown.",
    )
    parser_errors.set_defaults(func=command_list_errors)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
