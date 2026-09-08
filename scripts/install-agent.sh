#!/usr/bin/env bash
# PudimNetMon agent installer (Linux / systemd).
#
# Idempotent post-build installer for a PudimNetMon agent host.
#   1. Installs the hardened systemd unit (agent/systemd/pudim-agent.service)
#      into the systemd unit directory. The unit is refreshed on re-run so
#      upgrades pick up unit changes.
#   2. Seeds /etc/pudim/agent.conf from agent/config/agent.conf.example only
#      when no config file exists. An admin-edited agent.conf is never
#      overwritten, so re-running is safe and is the upgrade path.
#   3. Reloads systemd, then enables and starts the pudim-agent service.
#
# The script does not build the agent. Build and install the binary first.
#   cmake -S agent -B build && cmake --build build -j$(nproc)
#   sudo cmake --install build                     # -> /usr/local/bin/pudim-agent
#   sudo scripts/install-agent.sh
#
# Run from anywhere in the repo checkout. Re-running is safe. An existing
# /etc/pudim/agent.conf is left untouched and a running service is not
# restarted (run 'sudo systemctl restart pudim-agent' to apply new settings).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The unit's ExecStart is /usr/local/bin/pudim-agent (see agent/systemd/
# pudim-agent.service). --agent-bin overrides it for non-default prefixes.
AGENT_BIN="/usr/local/bin/pudim-agent"
SYSTEMD_DIR="/etc/systemd/system"
CONF_DIR="/etc/pudim"
NO_SERVICE=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: sudo scripts/install-agent.sh [options]

Idempotent Linux/systemd installer for the PudimNetMon agent. Installs the
hardened systemd unit, seeds /etc/pudim/agent.conf from
agent/config/agent.conf.example when no config exists yet (an existing config
is never overwritten), then enables and starts the service. Build the binary
first with cmake --install (see README "Agent on target hosts").

Options:
  --agent-bin=FILE   binary the unit's ExecStart must resolve to
                     (default: /usr/local/bin/pudim-agent)
  --systemd-dir=DIR  systemd unit directory (default: /etc/systemd/system)
  --config-dir=DIR   directory for the live agent.conf (default: /etc/pudim)
  --no-service       skip daemon-reload / enable / start (chroots, containers)
  --dry-run          print what would be done without changing anything
  -h, --help         show this help
EOF
}

for arg in "$@"; do
  case "$arg" in
    --agent-bin=*)   AGENT_BIN="${arg#*=}" ;;
    --systemd-dir=*) SYSTEMD_DIR="${arg#*=}" ;;
    --config-dir=*)  CONF_DIR="${arg#*=}" ;;
    --no-service)    NO_SERVICE=1 ;;
    --dry-run)       DRY_RUN=1 ;;
    -h|--help)       usage; exit 0 ;;
    *) echo "install-agent.sh: unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

UNIT_SRC="$ROOT/agent/systemd/pudim-agent.service"
CONF_SRC="$ROOT/agent/config/agent.conf.example"
UNIT_DST="$SYSTEMD_DIR/pudim-agent.service"
CONF_DST="$CONF_DIR/agent.conf"

# The unit's ExecStart must point at a real binary or the service fails on
# start. Enforce the documented build-then-install order.
if [[ ! -x "$AGENT_BIN" ]]; then
  echo "error: agent binary not found at $AGENT_BIN" >&2
  echo "Build and install it first, or pass --agent-bin=<path>:" >&2
  echo "  cmake -S agent -B build && cmake --build build -j\$(nproc)" >&2
  echo "  sudo cmake --install build" >&2
  exit 1
fi

for f in "$UNIT_SRC" "$CONF_SRC"; do
  if [[ ! -f "$f" ]]; then
    echo "error: missing $f (run from the repo checkout?)" >&2
    exit 1
  fi
done

if [[ $EUID -ne 0 ]] && [[ $DRY_RUN -eq 0 ]] && [[ $NO_SERVICE -eq 0 ]]; then
  echo "install-agent.sh: run as root (e.g. 'sudo scripts/install-agent.sh'), or" >&2
  echo "  pass --dry-run to preview, or --no-service to skip the systemctl steps" >&2
  exit 1
fi

echo "==> PudimNetMon agent installer"
echo "    unit : $UNIT_SRC -> $UNIT_DST"
echo "    conf : $CONF_SRC -> $CONF_DST (only if absent)"

# ---- 1. systemd unit (refreshed on every run so upgrades pick up changes) ----
unit_changed=0
if [[ -f "$UNIT_DST" ]] && cmp -s "$UNIT_SRC" "$UNIT_DST"; then
  echo "==> systemd unit unchanged: $UNIT_DST"
elif [[ $DRY_RUN -eq 1 ]]; then
  echo "==> [dry-run] install systemd unit: $UNIT_SRC -> $UNIT_DST"
  unit_changed=1
else
  install -d -m 0755 "$SYSTEMD_DIR"
  install -m 0644 "$UNIT_SRC" "$UNIT_DST"
  unit_changed=1
  echo "==> installed systemd unit: $UNIT_DST"
fi

# ---- 2. live config (seeded only when absent; never clobber admin edits) ----
if [[ -e "$CONF_DST" ]]; then
  echo "==> keeping existing config: $CONF_DST"
  echo "    (the installer never overwrites an existing agent.conf)"
elif [[ $DRY_RUN -eq 1 ]]; then
  echo "==> [dry-run] seed config: $CONF_SRC -> $CONF_DST"
else
  install -d -m 0755 "$CONF_DIR"
  install -m 0644 "$CONF_SRC" "$CONF_DST"
  echo "==> seeded config: $CONF_DST"
fi

# ---- 3. reload systemd, enable and start the service ----
if [[ $NO_SERVICE -eq 1 ]]; then
  echo "==> skipping systemd (--no-service)"
elif [[ $DRY_RUN -eq 1 ]]; then
  echo "==> [dry-run] systemctl daemon-reload"
  echo "==> [dry-run] systemctl enable --now pudim-agent.service"
else
  systemctl daemon-reload
  systemctl enable --now pudim-agent.service
  echo "==> enabled + started pudim-agent.service"
  if [[ $unit_changed -eq 1 ]] && systemctl is-active --quiet pudim-agent.service; then
    echo "==> note: unit replaced but the service still runs the old definition;"
    echo "    run 'sudo systemctl restart pudim-agent' to apply it."
  fi
fi

echo ""
echo "Next steps:"
echo "  sudoedit $CONF_DST   # collector-endpoints, node-id, probes, mTLS..."
echo "  sudo systemctl restart pudim-agent"
echo "Verify: systemctl status pudim-agent.service"
