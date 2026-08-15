# Deployment Guide

The [README](../README.md#quick-start) covers the **docker-compose stack** (all
services on one host) and [Running on a LAN
Server](../README.md#running-on-a-lan-server) explains how to remap the
host-side ports. This guide covers what Compose does **not**:

| Component | Runs where | Recommended deployment |
|---|---|---|
| [Agent](#1-agent-on-target-hosts) | Every host / VPN endpoint you monitor | Bare-metal binary + systemd service |
| [Dashboard](#2-dashboard-standalone) | LAN server (or any web host) | Static build (`dist/`) + nginx |
| [Consumers](#3-kafka-consumers-storage--alert) | Next to Kafka / TimescaleDB | Bare-metal binary + systemd service |
| [Collector (bare metal)](#4-collector-bare-metal) | LAN server | Optional — for a split-host stack |

> The Compose file bundles a **demo agent** so `docker compose up` shows data
> immediately. For real monitoring you deploy agents directly onto the hosts
> you care about — you can run as many as you want, all pointing at the same
> collector(s).

---

## 1. Agent on Target Hosts

`pudim-agent` is a self-contained C++17 daemon. It has no runtime
configuration file — **everything is configured via CLI flags**, so each host
gets its own targets and identity.

### 1.0 Download from the dashboard (recommended)

When the collector has staged agent binaries (the docker-compose collector
image ships a Linux x86_64 build), the dashboard offers a **Deploy Agent**
page that downloads the binary straight from the collector and shows a
copy-paste install command for Linux, Windows and Docker.

1. Open the dashboard and select **Deploy Agent** in the sidebar.
2. Pick your host's platform and press **Download** (the download URL is
   `GET /api/agent/download?platform=…` on the collector HTTP port).
3. Copy the install command from the card and run it on the target host —
   it downloads the same binary, installs the matching runtime libraries
   (Debian/Ubuntu 24.04) and starts `pudim-agent` pointed at this collector.

The staged Linux binary is built inside the Ubuntu 24.04 collector image, so
it links against that image's gRPC/OpenSSL runtime libraries. The install
command installs them via `apt`; on other distros (or other Ubuntu releases)
build from source instead (`scripts/package-agents.sh` produces a binary for
the host it runs on, so running it on the collector host yields a binary that
matches your fleet).

For bare-metal collectors, stage binaries with:

```bash
./scripts/package-agents.sh /usr/share/pudim/agents
pudim-collector --agent-dist-dir=/usr/share/pudim/agents ...
```

The collector scans the dist directory **once at startup**, so stage binaries
before starting it (or restart it after staging new ones).

The collector serves a JSON manifest at `GET /api/agent/versions` (platforms,
sizes, sha256) and the binaries at `GET /api/agent/download`. Platforms
without a staged binary are simply absent from the manifest.

### 1.1 Build

Install the build prerequisites (Ubuntu 24.04 shown):

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake protobuf-compiler libprotobuf-dev libgrpc++-dev libgrpc-dev \
    protobuf-compiler-grpc build-essential pkg-config ca-certificates \
    libcurl4-openssl-dev libssl-dev
# Optional features (auto-detected at configure time):
sudo apt-get install -y libpcap-dev libsystemd-dev libsqlite3-dev
```

Build and install from the repository root:

```bash
cmake -S agent -B build
cmake --build build -j$(nproc)
sudo cmake --install build        # installs /usr/local/bin/pudim-agent
```

The CMake configure step prints which optional features are enabled:

| Dependency | Enables |
|---|---|
| `libpcap` | TCP handshake capture probe (disable per-agent with `--no-tcp-handshake`) |
| `libsystemd` | `sd_notify` → lets the shipped unit use `Type=notify` + watchdog |
| `SQLite3` | Persistent disk buffer (default path `/var/lib/pudim/pending.db`) |

You can also build a container image (`infra/docker/Dockerfile.agent`) and run
it on hosts that already run Docker — but for target hosts a native binary +
systemd service is lighter and integrates with journald and cgroups.

### 1.2 Configure

Full list from `pudim-agent --help`:

| Flag | Default | Purpose |
|---|---|---|
| `-c, --collector-endpoint` | `localhost:50051` | Primary collector gRPC endpoint |
| `-b, --collector-endpoints` | — | Comma-separated failover list, e.g. `collector-a:50051,collector-b:50052` |
| `-n, --node-id` | `agent-unknown` | Unique node id shown in the dashboard |
| `-i, --interval` | `5000` | Probe / upload interval in ms |
| `-d, --dns-targets` | — | Comma-separated DNS resolution targets |
| `-p, --tcp-targets` | — | Comma-separated `host:port` TCP connect targets |
| `-s, --tls-targets` | — | Comma-separated `host:port` TLS handshake targets |
| `-w, --http-targets` | — | Comma-separated HTTP(S) URLs |
| `-x, --http-protocols` | — | HTTP versions to measure: `http1.1,http2,http3` |
| `-g, --ping-targets` | — | Comma-separated ICMP ping targets (needs `CAP_NET_RAW`) |
| `-k, --ping-count` | `4` | Pings per target per interval |
| `-u, --ping-gap-ms` | `200` | Delay between individual pings in ms |
| `-y, --dns-expected` | — | Expected DNS records, e.g. `host=A:1.2.3.4` |
| `-a, --diagnostic-address` | — | Advertised diagnostic gRPC endpoint, e.g. `web-01.lan:50052` — enables dashboard diagnostics for this agent |
| `-z, --diagnostic-port` | `50052` | Local gRPC diagnostic server port |
| `-f, --max-buffer-size` | `200` | Max in-memory metric batches before dropping |
| `-j, --disk-buffer-path` | `/var/lib/pudim/pending.db` | SQLite spill path for collector outages |
| `-l, --disk-buffer-max-mb` | `100` | Max disk buffer size |
| `-m, --stream-metrics` | off | Use client-streaming RPC instead of unary |
| `-C, --tls-ca` | — | PEM CA to verify the collector (mTLS) |
| `-E, --tls-cert` | — | PEM client certificate (mTLS) |
| `-K, --tls-key` | — | PEM client private key (mTLS) |
| `-t, --trace-id` | — | Static trace id for request correlation |
| `-v, --version` | `0.1.0` | Agent version string reported in heartbeats |
| `--no-tls-cert` | off | Disable the TLS certificate validation probe |
| `--no-tcp-retransmit` | off | Disable the TCP retransmission probe |
| `--no-tcp-handshake` | off | Disable the libpcap TCP handshake capture probe |
| `--tcp-handshake-interval` | `0` (every cycle) | Run the pcap handshake capture at most this often (ms) |
| `--log-level` | `info` | Log verbosity: `debug`, `info`, `warn`, `error` |
| `--no-tls-cert` | on | Disable the TLS certificate validation probe |
| `--no-tcp-retransmit` | on | Disable the TCP retransmission probe |
| `--no-tcp-handshake` | on | Disable the libpcap TCP handshake capture |

### 1.3 Install as a systemd service

The repo ships a hardened unit — [`agent/systemd/pudim-agent.service`](../agent/systemd/pudim-agent.service) — running unprivileged
(`User=nobody`), with `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome`, `PrivateTmp`, `PrivateDevices`, ambient
`CAP_NET_RAW CAP_NET_ADMIN` (ICMP + pcap only), `MemoryMax=256M`,
`CPUQuota=50%`, journald logging and a restart policy.

```bash
sudo cp agent/systemd/pudim-agent.service /etc/systemd/system/
sudo mkdir -p /etc/pudim /var/lib/pudim

# Point the unit at your collector and pick probe targets:
sudo systemctl edit --force --full pudim-agent
```

Override the `ExecStart` with your site-specific flags (example):

```ini
[Unit]
Description=PudimNetMon Agent - Network monitoring heartbeat daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
NotifyAccess=main
WatchdogSec=60
ExecStart=/usr/local/bin/pudim-agent \
    --collector-endpoints=collector.lan:50051,collector2.lan:50052 \
    --node-id=web-01 \
    --interval=5000 \
    --dns-targets=google.com,example.com \
    --tcp-targets=example.com:443,10.0.0.1:22 \
    --tls-targets=example.com:443 \
    --http-targets=https://example.com \
    --http-protocols=http1.1,http2 \
    --ping-targets=1.1.1.1 \
    --diagnostic-address=web-01.lan:50052

User=nobody
Group=nogroup
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN

LimitNOFILE=65536
MemoryMax=256M
CPUQuota=50%

StandardOutput=journal
StandardError=journal

Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

> `%H` in the shipped unit expands to the machine hostname — handy when you want
> the dashboard to identify agents by hostname. To change only the collector
> endpoint and targets, drop an override in
> `/etc/systemd/system/pudim-agent.service.d/override.conf` instead of copying
> the whole unit.

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now pudim-agent
systemctl status pudim-agent
journalctl -u pudim-agent -f      # JSON-structured logs on stdout
```

**Disk buffer:** make sure `/var/lib/pudim` is writable (the unit uses
`ProtectSystem=strict`; add `ReadWritePaths=/var/lib/pudim` to the override or
point `--disk-buffer-path` at an allowed location).

### 1.4 Enable mTLS (recommended for remote hosts)

Generate the CA + per-service certs once on the LAN server, then copy the
`ca.crt` + the agent cert/key to every target host:

```bash
# On the LAN server:
./scripts/gen-certs.sh certs    # → ca.crt, collector.crt/.key, agent.crt/.key

# To each target host (secure channel, e.g. scp over SSH):
scp certs/ca.crt   user@host:/etc/pudim/
scp certs/agent.crt user@host:/etc/pudim/
scp certs/agent.key user@host:/etc/pudim/   # chmod 600, root-owned
```

Add to the unit's `ExecStart`:

```
--tls-ca=/etc/pudim/ca.crt --tls-cert=/etc/pudim/agent.crt --tls-key=/etc/pudim/agent.key
```

The collector must be started with the matching server cert (`--tls-ca`,
`--tls-cert`, `--tls-key`), or the agent falls back to plaintext and logs the
effective transport at startup. See [`certificate-rotation.md`](certificate-rotation.md) for
the lifecycle.

### 1.5 Rolling out to many hosts

There is no central agent manager — each host runs its own unit. Good
practices:

1. Use **Ansible / Puppet / etc.** to push the binary, the unit, the certs and
   the `ExecStart` override; make `--node-id` unique per host (or rely on `%H`).
2. Add the **secondary collector** to `--collector-endpoints` so hosts fail
   over automatically (3-strike failover, in-memory → disk buffer → drain).
3. Add `--diagnostic-address=<host>:50052` (and open the firewall port) if you
   want traceroute/pcap diagnostics from the dashboard.
4. Verify the agents registered: `curl http://<server>:<PUDIM_COLLECTOR_HTTP_PORT>/agents`.

---

## 2. Dashboard (standalone)

The dashboard is a **static React build** — the only dynamic part is `/api/*`,
which nginx reverse-proxies to the collector HTTP endpoint. This means it can
be served from any web server or static host, not just the Compose nginx.

### 2.1 Build

```bash
cd dashboard
npm ci
npm run build          # outputs dist/
```

### 2.2 Serve behind nginx

Copy `dist/` anywhere (e.g. `/var/www/pudim/dashboard`) and point `/api/*` at
your collector — use the collector's LAN address, **not** the Compose service
name `collector`:

```nginx
server {
    listen 3000;
    server_name mon.lan;
    root /var/www/pudim/dashboard;
    index index.html;

    location /api/ {
        proxy_pass http://collector.lan:8080;   # <-- your collector
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    # Content-hashed build assets are immutable — cache aggressively.
    location /assets/ {
        expires 1y;
        add_header Cache-Control "public, immutable";
    }

    location / {
        try_files $uri $uri/ /index.html;
    }
}
```

The repo's [`dashboard/nginx.conf`](../dashboard/nginx.conf) is the same
configuration used inside Compose. Equivalent snippets work for Apache, Caddy,
or any reverse proxy. You can also reuse the Dockerfile directly:
`docker build -t pudim-dashboard dashboard/`.

### 2.3 What needs to be reachable

| From | To | Port |
|---|---|---|
| Browser → dashboard | web server | `3000` (or yours) |
| web server → collector `/api/*` | collector HTTP | `8080` (or `PUDIM_COLLECTOR_HTTP_PORT`) |

---

## 3. Kafka Consumers (storage + alert)

`pudim-consumer-storage` persists the `network.metrics` topic to TimescaleDB;
`pudim-consumer-alert` evaluates alert rules from the same stream. Both are
C++ binaries built from the collector CMake project and only need Kafka (the
storage consumer also needs TimescaleDB).

### 3.1 Build

```bash
sudo apt-get install -y cmake protobuf-compiler libprotobuf-dev libgrpc++-dev \
    libgrpc-dev protobuf-compiler-grpc build-essential pkg-config \
    libpq-dev librdkafka-dev

cmake -S collector -B build
cmake --build build -j$(nproc) --target pudim-consumer-storage pudim-consumer-alert
sudo cmake --install build      # installs pudim-collector + both consumers
```

### 3.2 Storage consumer

```
Usage: pudim-consumer-storage [options]
  --kafka-brokers  Kafka bootstrap servers (e.g. localhost:9092)
  --topic          Topic to consume (default: network.metrics)
  --group          Consumer group (default: storage)
  --http-addr      Prometheus listen address (default: 0.0.0.0:9091)
  --db-host        TimescaleDB host (default: localhost)
  --db-port        TimescaleDB port (default: 5432)
  --db-name        TimescaleDB database (default: pudimnetmon)
  --db-user        TimescaleDB user (default: pudim)
  --db-password    TimescaleDB password (default: pudim)
  --help           Show this help
```

Example:

```bash
pudim-consumer-storage \
    --kafka-brokers=kafka.lan:9092 \
    --db-host=db.lan --db-port=5432 --db-name=pudimnetmon \
    --db-user=pudim --db-password=<secret> \
    --http-addr=0.0.0.0:9091
```

### 3.3 Alert consumer

```
Usage: pudim-consumer-alert [options]
  --kafka-brokers      Kafka bootstrap servers (e.g. localhost:9092)
  --topic              Topic to consume (default: network.metrics)
  --group              Consumer group (default: alert)
  --http-addr          Prometheus listen address (default: 0.0.0.0:9092)
  --alert-rules-path   JSON file with alert rules (required)
  --help               Show this help
```

Example:

```bash
pudim-consumer-alert \
    --kafka-brokers=kafka.lan:9092 \
    --alert-rules-path=/etc/pudim/alert_rules.json \
    --http-addr=0.0.0.0:9093
```

Copy the rules file from the repo:
`sudo cp collector/config/alert_rules.json /etc/pudim/`.

### 3.4 systemd units

Two equivalent units; here are the `[Service]` sections:

```ini
# /etc/systemd/system/pudim-consumer-storage.service
[Unit]
Description=PudimNetMon storage consumer (Kafka → TimescaleDB)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/pudim-consumer-storage \
    --kafka-brokers=kafka.lan:9092 \
    --db-host=db.lan --db-port=5432 --db-name=pudimnetmon \
    --db-user=pudim --db-password=<secret> \
    --http-addr=0.0.0.0:9091
User=pudim
Group=pudim
NoNewPrivileges=true
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

```ini
# /etc/systemd/system/pudim-consumer-alert.service
[Unit]
Description=PudimNetMon alert consumer (Kafka → alert rules)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/pudim-consumer-alert \
    --kafka-brokers=kafka.lan:9092 \
    --alert-rules-path=/etc/pudim/alert_rules.json \
    --http-addr=0.0.0.0:9093
User=pudim
Group=pudim
NoNewPrivileges=true
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now pudim-consumer-storage pudim-consumer-alert
```

---

## 4. Collector (bare metal)

The LAN Server README section assumes the collector runs in Docker. You can
equally run it directly on the server — useful when spreading the stack across
hosts (Kafka/DB elsewhere) or when you don't want the container runtime.

```bash
cmake -S collector -B build
cmake --build build -j$(nproc)
sudo cmake --install build    # /usr/local/bin/pudim-collector

pudim-collector \
    --grpc-addr=0.0.0.0:50051 \
    --http-addr=0.0.0.0:8080 \
    --kafka-brokers=kafka.lan:9092 \
    --alert-rules-path=/etc/pudim/alert_rules.json
```

Flags (`pudim-collector --help`): `--grpc-addr`, `--http-addr`, `--db-*`,
`--kafka-brokers`, `--kafka-topic`, `--alert-rules-path`,
`--skew-threshold-ms`, `--backpressure-threshold-ms`, `--tls-ca/--tls-cert/--tls-key`.

> **In-Kafka mode** (`--kafka-brokers` set) the collector produces to the topic
> and the consumers own storage + alerting. **Without** `--kafka-brokers` it
> writes directly to TimescaleDB and evaluates alerts inline — a simpler
> single-binary topology.

---

## Deployment decision table

| Topology | Components | When to use |
|---|---|---|
| Single host, Compose | Everything in `docker-compose.yml` | Demo, lab, small LANs |
| LAN server + remote agents | Compose stack on server + bare-metal agents on hosts | **Typical production** |
| Split hosts | Bare-metal collector, consumers, Kafka, DB each on their own box | Larger networks / DR |

