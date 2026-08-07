#!/usr/bin/env bash
# Simulate a Kafka slowdown: pause and resume the broker, then observe
# backpressure propagation (consumer lag, agent buffer drops).
# Usage: ./scripts/overload-kafka.sh [pause_seconds]
set -euo pipefail

PAUSE_S="${1:-30}"

echo "=== Kafka slowdown experiment ==="
echo "1. Pausing Kafka broker for ${PAUSE_S}s ..."
docker compose stop kafka
echo "   -> consumer lag should climb; agent buffer may fill and drop oldest."

sleep "$PAUSE_S"

echo "2. Resuming Kafka broker ..."
docker compose start kafka

echo "3. Waiting for consumers to catch up (lag -> 0) ..."
for i in $(seq 1 12); do
  LAG=$(curl -s localhost:9091/metrics 2>/dev/null | grep -E '^pudim_kafka_consumer_lag ' | awk '{print $2}')
  echo "   [${i}s] consumer lag = ${LAG:-n/a}"
  if [[ "${LAG:-1}" == "0" ]]; then
    echo "   Lag drained."
    break
  fi
  sleep 5
done

echo "Done. Verify no duplicate writes:"
echo "  SELECT count(*) FROM network_metrics WHERE agent_id='load-agent-0' AND seq=1;"
