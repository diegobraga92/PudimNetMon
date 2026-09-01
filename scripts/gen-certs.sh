#!/usr/bin/env bash
# PudimNetMon mTLS certificate bootstrap
#
# Generates a self-signed CA plus per-service certificates so the agent and
# collector can authenticate each other over gRPC (mutual TLS).
#
# Usage:
#   ./scripts/gen-certs.sh [OUT_DIR]        # default: ./certs
#
# Produces (in OUT_DIR):
#   ca.crt, ca.key          self-signed CA (keep ca.key offline)
#   collector.crt/.key      server cert used by the collector gRPC server
#   agent.crt/.key          client cert used by the agent
#
# Wire-up:
#   collector: --tls-ca certs/ca.crt --tls-cert certs/collector.crt --tls-key certs/collector.key
#   agent:     --tls-ca certs/ca.crt --tls-cert certs/agent.crt --tls-key certs/agent.key
#
# NOTE: certs/*.key are plaintext private keys. In production, provision them
# via a secret manager / cert-manager instead. See docs/certificate-rotation.md.
set -euo pipefail

OUT="${1:-certs}"
DAYS_CA=3650      # long-lived CA
DAYS_LEAF=365     # leaf certs are rotated yearly
RSA_BITS=2048

mkdir -p "$OUT"

echo "==> Generating CA key + cert (valid ${DAYS_CA}d)"
openssl genrsa -out "$OUT/ca.key" "$RSA_BITS" 2>/dev/null
openssl req -x509 -new -key "$OUT/ca.key" -sha256 -days "$DAYS_CA" \
    -subj "/CN=PudimNetMon CA/O=PudimNetMon" -out "$OUT/ca.crt"

echo "==> Generating collector server cert (valid ${DAYS_LEAF}d)"
openssl genrsa -out "$OUT/collector.key" "$RSA_BITS" 2>/dev/null
openssl req -new -key "$OUT/collector.key" -sha256 \
    -subj "/CN=collector/O=PudimNetMon" -out "$OUT/collector.csr"
cat > "$OUT/collector.ext" <<EOF
subjectAltName = DNS:localhost, DNS:collector, DNS:collector-1, DNS:collector-2, IP:127.0.0.1
EOF
openssl x509 -req -in "$OUT/collector.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -CAcreateserial -days "$DAYS_LEAF" -sha256 \
    -extfile "$OUT/collector.ext" -out "$OUT/collector.crt"

echo "==> Generating agent client cert (valid ${DAYS_LEAF}d)"
openssl genrsa -out "$OUT/agent.key" "$RSA_BITS" 2>/dev/null
openssl req -new -key "$OUT/agent.key" -sha256 \
    -subj "/CN=agent/O=PudimNetMon" -out "$OUT/agent.csr"
# The agent also acts as the gRPC server for diagnostic RPCs, so its cert
# needs SANs matching how the collector addresses it (localhost / agent /
# 127.0.0.1).
cat > "$OUT/agent.ext" <<EOF
subjectAltName = DNS:localhost, DNS:agent, DNS:agent-1, DNS:agent-2, IP:127.0.0.1
EOF
openssl x509 -req -in "$OUT/agent.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -CAcreateserial -days "$DAYS_LEAF" -sha256 \
    -extfile "$OUT/agent.ext" -out "$OUT/agent.crt"

# Clean up intermediates
rm -f "$OUT"/*.csr "$OUT"/*.ext "$OUT"/*.srl

echo
echo "==> Done. Files:"
ls -1 "$OUT"
echo
echo "Collector (mTLS):"
echo "  --tls-ca $OUT/ca.crt --tls-cert $OUT/collector.crt --tls-key $OUT/collector.key"
echo "Agent (mTLS):"
echo "  --tls-ca $OUT/ca.crt --tls-cert $OUT/agent.crt --tls-key $OUT/agent.key"
