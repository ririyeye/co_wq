#!/usr/bin/env bash
set -euo pipefail

project_root="/home/ririyeye/co_wq_local"
log_dir="$project_root/logs"
mkdir -p "$log_dir"

tcpdump_log="$log_dir/quic_tcpdump.log"
tcpdump_pid=""

cd "$project_root"
# xmake build co_quic

server_trace="$project_root/logs/openssl_server.trace"
client_trace="$project_root/logs/openssl_client.trace"

rm -f "$server_trace" "$client_trace"

echo "[run_quic_sequence] capturing QUIC traffic (log => $tcpdump_log)"
tcpdump -i lo udp port 28545 -v -s 0 >"$tcpdump_log" 2>&1 &
tcpdump_pid=$!
sleep 0.2

install/bin/co_quic -l -v --tls-trace --auto --cert "$project_root/certs/server.crt" --key "$project_root/certs/server.key" &
pid1=$!

sleep 1

install/bin/co_quic --tls-trace --auto 127.0.0.1 &
pid2=$!

sleep 2

killall -SIGINT co_quic >/dev/null 2>&1 || true

wait "$pid1" 2>/dev/null || true
wait "$pid2" 2>/dev/null || true

if [[ -n "$tcpdump_pid" ]]; then
	kill "$tcpdump_pid" 2>/dev/null || true
	wait "$tcpdump_pid" 2>/dev/null || true
fi
