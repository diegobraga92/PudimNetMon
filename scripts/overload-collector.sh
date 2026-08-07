#!/usr/bin/env bash
# Simulate collector overload by spawning N agents at short intervals.
# Usage: ./scripts/overload-collector.sh [N] [interval_ms]
set -euo pipefail

N="${1:-10}"
INTERVAL="${2:-500}"

AGENT_BIN="$(cd "$(dirname "$0")/.." && pwd)/build-agent/pudim-agent"
COLLECTOR_EP="${COLLECTOR_EP:-localhost:50051}"

if [[ ! -x "$AGENT_BIN" ]]; then
  echo "ERROR: $AGENT_BIN not found. Build the agent first (cmake -S agent -B build-agent)." >&2
  exit 1
fi

echo "Starting $N agents (interval=${INTERVAL}ms) against $COLLECTOR_EP ..."
PIDS=()
for ((i = 0; i < N; i++)); do
  "$AGENT_BIN" \
    --collector-endpoint="$COLLECTOR_EP" \
    --node-id="load-agent-$i" \
    --interval="$INTERVAL" \
    --dns-targets=example.com \
    --tcp-targets=example.com:443 \
    --tls-targets=example.com:443 \
    --http-targets=https://example.com \
    --ping-count=0 \
    >"/tmp/overload-agent-$i.log" 2>&1 &
  PIDS+=("$!")
done

trap 'echo; echo "Stopping agents..."; kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true' EXIT

echo "Running (Ctrl-C to stop). Watch the collector:"
echo "  curl -s localhost:8080/metrics | grep pudim_metrics_received_total"
echo "  curl -s localhost:8080/metrics | grep pudim_backpressure_signals_sent_total"
wait
