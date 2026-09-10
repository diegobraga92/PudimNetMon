#!/usr/bin/env bash
# Builds the Linux agent release artifact and packages it into:
#   * a tarball   (for the one-command install-linux.sh downloader), and
#   * a single-file self-extracting .run installer that carries the binary and
#     reads an optional `-cfg-<base64url>` config token from its own file name
#     (as served by the dashboard) so the collector/node are applied for free.
#
# The agent links OpenSSL/cURL/libpcap dynamically (they use dlopen and NSS at
# runtime, so a fully static glibc binary is not possible), but gRPC, protobuf
# and abseil are linked statically (PUDIM_STATIC_GRPC). Their shared-library
# sonames are not stable across Ubuntu releases.
#
# Usage:
#   scripts/build-linux-release.sh [options]
#
# Options:
#   --host    build on the current host (use the installed toolchain)
#   --docker  build inside ubuntu:24.04 for reproducibility (default when
#             docker is available and the source tree is clean enough)
#   -h, --help
#
# Output (in dist/):
#   pudimnetmon-agent-<version>-linux-<arch>.tar.gz
#   pudimnetmon-agent-<version>-linux-<arch>.tar.gz.sha256
#   pudimnetmon-agent-install-<version>-linux-<arch>.run
#   pudimnetmon-agent-install-<version>-linux-<arch>.run.sha256
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ---- version and arch -------------------------------------------------------
VERSION="$(sed -n 's/.*project(pudim-agent VERSION \([0-9][0-9.]*\)).*/\1/p' agent/CMakeLists.txt)"
VERSION="${VERSION:-0.1.0}"

case "$(uname -m)" in
  x86_64)  ARCH="amd64" ;;
  aarch64) ARCH="arm64" ;;
  *)       ARCH="$(uname -m)" ;;
esac

BUILD_MODE="auto"
for arg in "$@"; do
  case "$arg" in
    --host)   BUILD_MODE="host" ;;
    --docker) BUILD_MODE="docker" ;;
    -h|--help) awk 'NR >= 2 && /^set -euo/ { exit } NR >= 2 { print }' "$0" |
                sed -n 's/^# \{0,1\}//p'; exit 0 ;;
    *) echo "build-linux-release.sh: unknown option: $arg" >&2; exit 2 ;;
  esac
done

mkdir -p dist

# ---- build ------------------------------------------------------------------
build_host() {
  echo "==> Building agent (host toolchain, Release, static gRPC/protobuf/abseil)"
  cmake -S agent -B build-agent-release -DCMAKE_BUILD_TYPE=Release -DPUDIM_STATIC_GRPC=ON
  cmake --build build-agent-release -j"$(nproc)"
  cp build-agent-release/pudim-agent dist/pudim-agent-linux-${ARCH}
}

build_docker() {
  echo "==> Building agent inside ubuntu:24.04 (reproducible)"
  # Uses the repo root as context because agent/CMakeLists.txt references
  # ../api/proto for the .proto files.
  local image="pudimnetmon-agent-builder"
  docker build -t "$image" -f - . <<'EOF'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y -qq \
    cmake protobuf-compiler libprotobuf-dev libgrpc++-dev \
    libgrpc-dev protobuf-compiler-grpc build-essential \
    pkg-config git ca-certificates \
    libcurl4-openssl-dev libssl-dev libpcap-dev libsystemd-dev libsqlite3-dev
WORKDIR /build
COPY agent/ /build/agent/
COPY api/ /build/api/
RUN cmake -S agent -B build -DCMAKE_BUILD_TYPE=Release -DPUDIM_STATIC_GRPC=ON && \
    cmake --build build -j$(nproc)
EOF
  docker create --name pudimnetmon-agent-builder-c "$image" /bin/true >/dev/null
  docker cp pudimnetmon-agent-builder-c:/build/build/pudim-agent "dist/pudim-agent-linux-${ARCH}"
  docker rm pudimnetmon-agent-builder-c >/dev/null
  docker rmi "$image" >/dev/null 2>&1 || true
}

if [ "$BUILD_MODE" = "host" ]; then
  build_host
elif [ "$BUILD_MODE" = "docker" ]; then
  build_docker
else
  if command -v docker >/dev/null 2>&1; then
    build_docker
  else
    build_host
  fi
fi
if [ ! -f "dist/pudim-agent-linux-${ARCH}" ]; then
  echo "error: dist/pudim-agent-linux-${ARCH} not found (build failed)" >&2
  exit 1
fi

# ---- package -----------------------------------------------------------------
echo "==> Packaging dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}.tar.gz"
STAGE="dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Layout matches the Docker runtime image:
#   pudim-agent      the ELF binary
#   pudim-agent.service
#   agent.conf.example
#   version.txt
cp "dist/pudim-agent-linux-${ARCH}" "$STAGE/pudim-agent"
chmod +x "$STAGE/pudim-agent"
cp agent/systemd/pudim-agent.service "$STAGE/pudim-agent.service"
cp agent/config/agent.conf.example "$STAGE/agent.conf.example"
printf '%s\n' "$VERSION" > "$STAGE/version.txt"

tar -czf "dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}.tar.gz" -C dist "$(basename "$STAGE")"
sha256sum "dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}.tar.gz" \
  > "dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}.tar.gz.sha256"
# ---- single-file .run installer ---------------------------------------------
# Self-extracting: a bash wrapper (config-token decode + self-extraction) with
# install-linux.sh and the agent binary appended as a gzip payload. The wrapper
# decodes an optional `-cfg-<base64url>` token from its own file name (the name
# the dashboard serves) and applies collector/node/interval before CLI flags.
RUN_NAME="pudimnetmon-agent-install-${VERSION}-linux-${ARCH}.run"
echo "==> Packaging dist/${RUN_NAME}"
RUN_STAGE="$(mktemp -d)"
trap 'rm -rf "$RUN_STAGE"' EXIT
cp scripts/install-linux.sh "$RUN_STAGE/install-linux.sh"
cp "dist/pudim-agent-linux-${ARCH}" "$RUN_STAGE/pudim-agent"
chmod +x "$RUN_STAGE/pudim-agent" "$RUN_STAGE/install-linux.sh"
tar -czf "$RUN_STAGE/payload.tar.gz" -C "$RUN_STAGE" pudim-agent install-linux.sh

cat > "$RUN_STAGE/wrapper.sh" <<'RUN_WRAPPER'
#!/usr/bin/env bash
# PudimNetMon agent - Linux single-file installer.
#
# Self-extracting .run: contains the agent binary and install-linux.sh. When
# the dashboard served the download, the file name carries a config token:
#
#   pudimnetmon-agent-install-0.1.0-linux-amd64-cfg-<base64url>.run
#
# The token decodes to a query string (collector=host:port&node=..&interval=..)
# and is applied automatically. Explicit CLI flags always win over the token.
#
# Usage:
#   sudo ./pudimnetmon-agent-install-<version>-linux-<arch>.run [install-linux.sh options]
set -euo pipefail

# Re-run as root when possible; install-linux.sh needs it for apt/systemd.
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
  exec sudo "$0" "$@"
fi

SELF="$0"
NAME="$(basename "$SELF")"
# Built from two literals so the full marker text appears only on the payload
# boundary line (grep -abo below must match exactly once in the wrapper).
MARKER="#__PUDIM_""PAYLOAD__"
COLLECTOR=""
NODE=""
INTERVAL=""

# ---- decode optional config token from the file name ------------------------
if [[ "$NAME" == *-cfg-* ]]; then
  TOKEN="${NAME%.run}"          # strip the .run suffix
  TOKEN="${TOKEN#*-cfg-}"       # keep what follows the first "-cfg-"
  if [[ "$TOKEN" =~ ^[A-Za-z0-9_-]+$ ]] && command -v base64 >/dev/null 2>&1; then
    # base64url -> base64 (padding) -> raw query string
    B64="${TOKEN//-/+}"
    B64="${B64//_//}"
    case $(( ${#B64} % 4 )) in
      2) B64="${B64}==" ;;
      3) B64="${B64}=" ;;
    esac
    QUERY="$(printf '%s' "$B64" | base64 -d 2>/dev/null || true)"
    # URL-decode values and split key=value&key=value pairs.
    url_decode() {
      local enc="$1" out="" hex
      enc="${enc//+/ }"
      while [[ "$enc" == *%* ]]; do
        out+="${enc%%%*}"
        enc="${enc#*%}"
        hex="${enc:0:2}"
        out+="$(printf "\\x${hex}")"
        enc="${enc:2}"
      done
      out+="$enc"
      printf '%s' "$out"
    }
    IFS='&' read -r -a PAIRS <<< "$QUERY"
    for PAIR in "${PAIRS[@]}"; do
      KEY="${PAIR%%=*}"
      VAL="${PAIR#*=}"
      case "$KEY" in
        collector) COLLECTOR="$(url_decode "$VAL")" ;;
        node)      NODE="$(url_decode "$VAL")" ;;
        interval)  INTERVAL="$(url_decode "$VAL")" ;;
      esac
    done
    if [ -n "$COLLECTOR" ]; then
      echo "==> Config token detected in file name (collector=${COLLECTOR})"
    fi
  fi
fi

# ---- self-extract payload ---------------------------------------------------
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
# Byte offset of the marker (grep -abo prints 0-based match position). Payload
# starts right after the marker line: position + marker length + 1 newline.
MARK_OFF="$(grep -abo "$MARKER" "$SELF" | head -1 | cut -d: -f1)"
if [ -z "$MARK_OFF" ]; then
  echo "error: $SELF is missing its payload marker" >&2
  exit 2
fi
START=$((MARK_OFF + ${#MARKER} + 2))   # tail -c is 1-based
tail -c +"$START" "$SELF" | tar -xzf - -C "$TMP_DIR"
chmod +x "$TMP_DIR/pudim-agent" "$TMP_DIR/install-linux.sh"

# ---- run the embedded installer ---------------------------------------------
ARGS=(--binary="$TMP_DIR/pudim-agent")
[ -n "$COLLECTOR" ] && ARGS+=(--collector-endpoints="$COLLECTOR")
[ -n "$NODE" ]      && ARGS+=(--node-id="$NODE")
[ -n "$INTERVAL" ]  && ARGS+=(--interval="$INTERVAL")
exec bash "$TMP_DIR/install-linux.sh" "${ARGS[@]}" "$@"
#__PUDIM_PAYLOAD__
RUN_WRAPPER

# Assemble: wrapper text (ending in the payload marker) + gzip payload bytes.
{
  cat "$RUN_STAGE/wrapper.sh"
  cat "$RUN_STAGE/payload.tar.gz"
} > "dist/$RUN_NAME"
chmod +x "dist/$RUN_NAME"
sha256sum "dist/$RUN_NAME" > "dist/$RUN_NAME.sha256"

echo "==> Done:"
ls -la "dist/pudimnetmon-agent-${VERSION}-linux-${ARCH}.tar.gz" \
      "dist/${RUN_NAME}"
