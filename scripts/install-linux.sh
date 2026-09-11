#!/usr/bin/env bash
# PudimNetMon agent - Linux one-command installer.
#
# Installs the prebuilt agent binary as a hardened systemd service on
# Debian/Ubuntu. It is repo-independent: everything except the binary is
# embedded here, so it works when piped from curl.
#
#   curl -fsSL https://raw.githubusercontent.com/diegobraga92/PudimNetMon/main/scripts/install-linux.sh \
#     | sudo bash -s -- --collector-endpoints=collector.lan:50051 --node-id=host-01
#
# The agent links gRPC/protobuf/OpenSSL/cURL, which are not fully static on
# glibc, so the matching runtime libraries must be present. On Debian/Ubuntu
# this installer installs them with apt-get (same set as the agent Docker
# image). On other distros it stops and points to the container or source
# build. The binary itself is fetched from PUDIM_AGENT_URL (GitHub release by
# default) or passed with --binary.
#
# Options:
#   --binary=FILE           install a local agent binary instead of downloading
#   --collector-endpoints=  comma-separated collector list (writes agent.conf)
#   --node-id=              node identifier (defaults to the hostname)
#   --interval=MS           probe cadence in ms (default 5000)
#   --config-dir=DIR        directory for agent.conf (default /etc/pudim)
#   --systemd-dir=DIR       systemd unit directory (default /etc/systemd/system)
#   --no-service            skip daemon-reload / enable / start
#   --dry-run               print actions without changing the system
#   --uninstall             stop, disable and remove the agent + unit
#   -h, --help
#
# Exit status is 0 on success.
set -euo pipefail

# ---- defaults ---------------------------------------------------------------
case "$(uname -m)" in
  x86_64)  ARCH_SUFFIX="linux-amd64" ;;
  aarch64|arm64) ARCH_SUFFIX="linux-arm64" ;;
  *)       ARCH_SUFFIX="linux-$(uname -m)" ;;
esac
PUDIM_AGENT_URL="${PUDIM_AGENT_URL:-https://github.com/diegobraga92/PudimNetMon/releases/latest/download/pudimnetmon-agent-${ARCH_SUFFIX}.tar.gz}"
BINARY_FILE=""
COLLECTOR_ENDPOINTS=""
NODE_ID="$(hostname 2>/dev/null || echo pudim-agent)"
INTERVAL_MS="5000"
CONF_DIR="/etc/pudim"
SYSTEMD_DIR="/etc/systemd/system"
NO_SERVICE=0
DRY_RUN=0
UNINSTALL=0
SUDO_PREFIX=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then SUDO_PREFIX="sudo"; else SUDO_PREFIX=""; fi
fi
# ---- embedded systemd unit (mirrors agent/systemd/pudim-agent.service) ------
# Deliberately no --node-id on ExecStart. The installer seeds node-id in
# agent.conf, and precedence is defaults < agent.conf < service command line.
# Baking a CLI node-id here would override the config.
UNIT_CONTENT='[Unit]
Description=PudimNetMon Agent
Documentation=https://github.com/diegobraga92/PudimNetMon
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
NotifyAccess=main
WatchdogSec=60
ExecStart=/usr/local/bin/pudim-agent

# Run unprivileged and retain only the capabilities the probes need
User=nobody
Group=nogroup
StateDirectory=pudim
StateDirectoryMode=0750
Environment=HOME=/var/lib/pudim
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN

# Resource limits. Note: CPUQuota throttles the external tools too, so the
# speedtest command reads roughly 10-15% lower than a bare-metal run on a fast
# link; raise/remove it if you need parity with a terminal run.
LimitNOFILE=65536
MemoryMax=256M
CPUQuota=50%

# stdout+stderr captured and rotated by journald
StandardOutput=journal
StandardError=journal
JournaldStreamRateIntervalSec=10

# Restart behavior
Restart=always
RestartSec=5
StartLimitInterval=300
StartLimitBurst=3

[Install]
WantedBy=multi-user.target'

CONF_FILE="${CONF_DIR}/agent.conf"

usage() {
  sed -n '2,32p' "$0" | sed -n 's/^# \{0,1\}//p'
}

# ---- parse args -------------------------------------------------------------
for arg in "$@"; do
  case "$arg" in
    --binary=*)              BINARY_FILE="${arg#*=}" ;;
    --collector-endpoints=*) COLLECTOR_ENDPOINTS="${arg#*=}" ;;
    --node-id=*)             NODE_ID="${arg#*=}" ;;
    --interval=*)            INTERVAL_MS="${arg#*=}" ;;
    --config-dir=*)          CONF_DIR="${arg#*=}" ;;
    --systemd-dir=*)         SYSTEMD_DIR="${arg#*=}" ;;
    --no-service)            NO_SERVICE=1 ;;
    --dry-run)               DRY_RUN=1 ;;
    --uninstall)             UNINSTALL=1 ;;
    -h|--help)               usage; exit 0 ;;
    *) echo "install-linux.sh: unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

log()    { printf '%s\n' "$*"; }

run_as_root() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '[dry-run] %s\n' "$*"
    return 0
  fi
  if [ -n "$SUDO_PREFIX" ]; then
    $SUDO_PREFIX "$@"
  else
    "$@"
  fi
}

# ---- uninstall path ---------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
  log "==> Stopping and disabling pudim-agent.service"
  run_as_root systemctl disable --now pudim-agent.service 2>/dev/null || true
  if [ -f "${SYSTEMD_DIR}/pudim-agent.service" ]; then
    run_as_root rm -f "${SYSTEMD_DIR}/pudim-agent.service"
    run_as_root systemctl daemon-reload 2>/dev/null || true
  fi
  if [ -f /usr/local/bin/pudim-agent ]; then
    run_as_root rm -f /usr/local/bin/pudim-agent
  fi
  log "==> Removed the agent and its systemd unit. Kept ${CONF_FILE}."
  log "    Remove it manually (sudo rm $CONF_FILE) to purge all local state."
  exit 0
fi

# ---- download or resolve the binary ------------------------------------------
if [ -z "$BINARY_FILE" ]; then
  log "==> Downloading pudimnetmon-agent (latest ${ARCH_SUFFIX} release)"
  log "    URL: $PUDIM_AGENT_URL"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "    [dry-run] skipping download"
  else
    command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 1; }
    TMP_DIR="$(mktemp -d)"
    curl -fsSL --retry 3 -o "$TMP_DIR/pkg.tar.gz" "$PUDIM_AGENT_URL"
    tar -xzf "$TMP_DIR/pkg.tar.gz" -C "$TMP_DIR"
    BINARY_FILE="$(find "$TMP_DIR" -type f -name pudim-agent | head -1)"
    if [ -z "$BINARY_FILE" ]; then
      echo "error: tarball did not contain a pudim-agent binary" >&2
      exit 1
    fi
  fi
fi

if [ "$DRY_RUN" -eq 0 ]; then
  if [ ! -f "$BINARY_FILE" ]; then
    echo "error: agent binary not found at $BINARY_FILE" >&2
    exit 1
  fi
  chmod +x "$BINARY_FILE"
fi

# ---- runtime library check ---------------------------------------------------
# gRPC/protobuf/OpenSSL are not static on glibc, so the target needs the same
# runtime libs the agent Docker image installs. Auto-install them on Ubuntu
# (the package names are Ubuntu 24.04 t64 names). On other distros, tell the
# user to use the container or a source build.
needs_runtime_libs() {
  if ! command -v ldd >/dev/null 2>&1; then return 1; fi
  ldd "$BINARY_FILE" 2>/dev/null | grep -q 'not found'
}

distro_id() {
  if [ -r /etc/os-release ]; then
    sed -n 's/^ID=//p' /etc/os-release | tr -d '"'
  else
    echo unknown
  fi
}

RUNTIME_LIBS="libcurl4t64 libssl3t64 libpcap0.8t64 libsystemd0 libsqlite3-0"

if [ "$DRY_RUN" -eq 0 ]; then
  if needs_runtime_libs; then
    if [ "$(distro_id)" = "ubuntu" ] && command -v apt-get >/dev/null 2>&1; then
      log "==> Installing runtime libraries (apt-get)"
      # shellcheck disable=SC2086
      run_as_root apt-get update
      # shellcheck disable=SC2086
      run_as_root apt-get install -y $RUNTIME_LIBS
      
      if needs_runtime_libs; then
        echo "error: the agent still has missing shared libraries after" >&2
        echo "installing: $RUNTIME_LIBS" >&2
        echo "       Inspect with: ldd $BINARY_FILE | grep 'not found'" >&2
        exit 1
      fi
    else
      echo "error: the prebuilt agent needs runtime libraries that are missing" >&2
      echo "on this host (OpenSSL/cURL/libpcap/libsystemd/sqlite3). Install them" >&2
      echo "manually, use the agent Docker image (infra/docker/Dockerfile.agent)," >&2
      echo "or build from source (see the README)." >&2
      exit 1
    fi
  else
    log "==> Runtime libraries already present"
  fi
fi

# ---- install -----------------------------------------------------------------
BIN_DST="/usr/local/bin/pudim-agent"

log "==> Installing agent"
if [ "$DRY_RUN" -eq 0 ]; then
  run_as_root install -m 0755 "$BINARY_FILE" "$BIN_DST"
  log "    $BIN_DST"

  run_as_root mkdir -p "$SYSTEMD_DIR"
  printf '%s\n' "$UNIT_CONTENT" | run_as_root tee "${SYSTEMD_DIR}/pudim-agent.service" >/dev/null
  log "    ${SYSTEMD_DIR}/pudim-agent.service"
else
  echo "    [dry-run] install $BINARY_FILE -> $BIN_DST"
  echo "    [dry-run] write unit -> ${SYSTEMD_DIR}/pudim-agent.service"
fi

# ---- config (seed only when absent) ------------------------------------------
CONF_DST="${CONF_DIR}/agent.conf"
if [ "$DRY_RUN" -eq 1 ]; then
  echo "    [dry-run] seed $CONF_DST (only if absent)"
elif [ -e "$CONF_DST" ]; then
  log "==> Keeping existing config: $CONF_DST"
else
  run_as_root mkdir -p "$CONF_DIR"
  CONF_TEXT="# Generated by the PudimNetMon agent installer
collector-endpoints=${COLLECTOR_ENDPOINTS:-collector.lan:50051}
node-id=${NODE_ID}
interval=${INTERVAL_MS}

# Optional probe targets (comma-separated lists)
#dns-targets=example.com
#tcp-targets=example.com:443
#tls-targets=example.com:443
#http-targets=https://example.com
#ping-targets=1.1.1.1
#ping-count=4
#ping-gap-ms=200

# Deep diagnostics
#diagnostic-port=50052
#diagnostic-address=
"
  printf '%s' "$CONF_TEXT" | run_as_root tee "$CONF_DST" >/dev/null
  run_as_root chmod 0644 "$CONF_DST"
  log "==> Seeded $CONF_DST"
fi

# ---- service ------------------------------------------------------------------
if [ "$NO_SERVICE" -eq 1 ]; then
  log "==> Skipping systemd (--no-service)"
elif [ "$DRY_RUN" -eq 1 ]; then
  echo "    [dry-run] systemctl daemon-reload"
  echo "    [dry-run] systemctl enable pudim-agent.service"
  echo "    [dry-run] systemctl restart pudim-agent.service"
else
  log "==> Reloading systemd and (re)starting pudim-agent.service"
  systemctl daemon-reload
  systemctl enable pudim-agent.service
  systemctl restart pudim-agent.service
  if systemctl is-active --quiet pudim-agent.service 2>/dev/null; then
    log "==> pudim-agent.service is running the installed build"
  else
    log "==> Started. Verify with: systemctl status pudim-agent.service"
  fi
fi

echo ""
echo "Next steps:"
echo "  sudoedit $CONF_DST   # collector-endpoints, node-id, probes, mTLS..."
echo "  sudo systemctl restart pudim-agent"
echo "Verify: systemctl status pudim-agent.service"

