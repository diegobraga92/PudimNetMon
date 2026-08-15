# Performance

## Agent resource profile

`pudim-agent` is an I/O-bound daemon. Measured on a typical Linux host (default
5 s interval, one target of each type, collector reachable):

| Resource | Measured |
|---|---|
| RSS | ~30 MB (bounded in-memory buffer + SQLite spill cap) |
| CPU | < 1% of one core in steady state |
| Threads | ~25–28 (gRPC-dominated; probe worker + small pool add ~5) |
| Disk I/O | ~0 in normal operation (SQLite WAL only on collector outage) |

Per-cycle work is kept low by reusing connections across cycles:

- **curl handle reuse** — persistent per-thread `CURL*` keeps libcurl's
  connection pool + DNS cache alive between HTTP probes.
- **Shared `SSL_CTX` + TLS session cache** — one context per process and
  abbreviated handshakes for repeated TLS probes (the certificate probe still
  performs a full handshake so it always sees a fresh certificate).
- **Cached, time-bounded DNS** — `DnsResolver` deduplicates the several
  lookups each cycle performs per host, caches successes for 60 s, and never
  blocks a probe longer than 3 s even if the resolver hangs. The DNS
  *resolution* probe always performs a fresh (uncached) lookup so its latency
  metric stays meaningful; the DNS *record* probe (A/AAAA/CNAME) is served
  from the 60 s cache, so a record change is detected within ~1 minute (tune
  `DnsResolver::kDefaultTtlMs` in `agent/src/dns_resolver.h` if needed).
- **Probe worker thread** — probes run off the main loop at a fixed cadence;
  a slow/hung probe (blackholed target, slow resolver) no longer delays
  heartbeats. Independent probes (DNS/TCP/TLS/HTTP) run in parallel on a small
  pool; ICMP and libpcap capture stay serialized.
- **Bounded connect timeouts** — every TCP connect and DNS lookup has a hard
  deadline, so a firewalled target cannot stall a cycle for the OS default
  (~2 min).

Every minute the agent logs a self-observability line:

```
self: cycle=110ms probes=12ms metrics=11 queued=0 handshake_runs=0
```

`--log-level` (debug|info|warn|error) controls per-cycle log verbosity.

## Dashboard Performance & Bundle Optimization

## Bundle optimization (done)

The dashboard is a Vite + React 18 + Recharts SPA. Recharts is the dominant
dependency (~550 kB min). Before Phase 8 the entire app shipped as one bundle.

**Change** (`dashboard/vite.config.ts`): manual chunking splits vendors into
independently-cacheable files.

| Build (production, minified) | Before | After |
|---|---|---|
| App bundle | `578.77 kB` (gzip 171.30 kB) | `20.96 kB` (gzip 6.47 kB) |
| React (react + react-dom) | included in app bundle | `0.00 kB`* |
| Recharts | included in app bundle | `557.75 kB` (gzip 165.50 kB) |

\* React is tree-shaken into the app entry; the chunk file is nominal.

Because recharts is a separate file, the browser caches it across deploys —
only the ~21 kB app chunk changes on a code update. The main-thread parse cost
of recharts is deferred/independent of app code.

Verified: `cd dashboard && npm run build` → 3 assets, app chunk 20.96 kB.

## Lighthouse audit

Lighthouse requires Chrome; it is **not run in this environment** (no
headless Chrome). To reproduce:

```bash
cd dashboard
npm run build
npm run preview &          # serves dist/ on :4173
npx lighthouse http://localhost:4173 --view
# or against the docker stack: npx lighthouse http://localhost:3000
```

Known low-hanging items for the audit:

1. **Fonts / color** — the dashboard uses system fonts; no web-font download.
2. **Largest Contentful Paint** — the React app renders the header immediately;
   charts appear once `/api/health` returns (local network → fast).
3. **Bundle size** — fixed by the chunk split above (the `>500 kB` Vite warning
   now points at the cached recharts chunk only).
4. **Accessibility** — add `aria-label`s to the chart `Brush` and config
   selects; the alert acknowledge button already has a text label.
5. **Service worker** — not configured; fine for a demo, worth adding for
   production caching.

## Suggested next steps (production)

- Lazy-load recharts via `React.lazy` so the chart sections only fetch the
  recharts chunk when they render.
- Add a service worker (Workbox) for app-shell caching.
- Set `chunkSizeWarningLimit` explicitly or accept the recharts warning.
