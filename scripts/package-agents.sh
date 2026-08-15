#!/usr/bin/env bash
# Builds the agent in release mode and stages the binary into a directory that
# the collector serves to the dashboard (--agent-dist-dir). Layout:
#
#   <out>/pudim-agent-linux-amd64
#   <out>/version.txt
#
# The Docker collector image runs this same convention (see
# infra/docker/Dockerfile.collector). For non-Docker deployments, run this
# script on the collector host and point --agent-dist-dir at <out>.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist/agents}"
VERSION="${PUDIM_AGENT_VERSION:-0.1.0}"

echo "==> Building agent (Release)"
cmake -S "$ROOT/agent" -B "$ROOT/build-agent-release" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-agent-release" -j"$(nproc)"

echo "==> Staging into $OUT"
mkdir -p "$OUT"
cp "$ROOT/build-agent-release/pudim-agent" "$OUT/pudim-agent-linux-amd64"
chmod +x "$OUT/pudim-agent-linux-amd64"
printf '%s\n' "$VERSION" > "$OUT/version.txt"

echo "==> Done: $OUT/pudim-agent-linux-amd64 (version $VERSION)"
ls -la "$OUT"
