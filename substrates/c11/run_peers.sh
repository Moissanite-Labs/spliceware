#!/bin/sh
# Parameterized two-peer UDP runner (replaces sacred-splicer/gen.sh, which
# hardcoded a missing genesis.c). Usage:
#   ./run_peers.sh <generation_binary> [port_a] [port_b]
set -e

if [ $# -lt 1 ]; then
    echo "usage: $0 <generation_binary> [port_a] [port_b]" >&2
    exit 2
fi

BIN="$1"
PORT_A="${2:-9001}"
PORT_B="${3:-9002}"

if [ ! -x "$BIN" ]; then
    echo "error: '$BIN' is not executable (build it first, e.g. make -C substrates/c11)" >&2
    exit 1
fi

"$BIN" "$PORT_A" "$PORT_B" &
PEER_A=$!
trap 'kill "$PEER_A" 2>/dev/null || true' EXIT INT TERM
sleep 0.2
"$BIN" "$PORT_B" "$PORT_A"
kill "$PEER_A" 2>/dev/null || true
trap - EXIT INT TERM
