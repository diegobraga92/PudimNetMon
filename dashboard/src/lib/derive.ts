import type { MetricPoint } from '../types'

/** A recharts-friendly time-series row: one row per time bucket with every series aligned. */
export interface SeriesPoint {
  time: string
  time_ms: number
  [series: string]: string | number
}

export interface SeriesOptions {
  /** Bucket width in ms. Defaults to the cadence heuristics (min 1000ms). */
  bucketMs?: number
  /** Prefix series labels with `check_type` (use for "all checks", where probes share agent + target). */
  includeCheckType?: boolean
  /** Fill empty buckets across this absolute span so outages render as gaps instead of dots. */
  rangeMs?: { from: number; to: number }
}

/** Even bucket widths; larger buckets smooth noisy probes, smaller ones keep fine detail. */
const BUCKET_STEPS_MS = [
  250, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000,
  300000, 600000, 900000, 1800000, 3600000,
]

const DEFAULT_CADENCE_MS = 1000
const MAX_BUCKETS = 360

/**
 * Pick an x-axis bucket that smooths probe jitter while keeping the chart bounded
 * to ~MAX_BUCKETS points across the selected window. When the fetched metrics show
 * a coarser cadence (e.g. an NTP probe every minute), the bucket widens to match so
 * every series still lands on roughly one row per cycle and lines stay connected.
 */
export function pickBucketMs(windowSeconds: number, metrics: MetricPoint[] = []): number {
  const spanMs = windowSeconds * 1000
  const bySeries = new Map<string, number[]>()
  for (const m of metrics) {
    if (!m.success) continue
    const key = `${m.check_type}\u0000${m.agent_id}\u0000${m.target}`
    const times = bySeries.get(key)
    if (times) times.push(m.time_ms)
    else bySeries.set(key, [m.time_ms])
  }
  const gaps: number[] = []
  for (const times of bySeries.values()) {
    if (times.length < 2) continue
    times.sort((a, b) => a - b)
    for (let i = 1; i < times.length; i++) gaps.push(times[i] - times[i - 1])
  }
  gaps.sort((a, b) => a - b)
  // Upper-quartile gap between samples of the same series = typical probe cadence.
  const cadence = gaps.length ? gaps[(gaps.length * 3) >> 2] : DEFAULT_CADENCE_MS
  const target = Math.max(cadence, spanMs / MAX_BUCKETS)
  return BUCKET_STEPS_MS.find((s) => s >= target) ?? BUCKET_STEPS_MS[BUCKET_STEPS_MS.length - 1]
}

function seriesLabel(m: MetricPoint, includeCheckType: boolean): string {
  return includeCheckType ? `${m.check_type} | ${m.agent_id} | ${m.target}` : `${m.agent_id} | ${m.target}`
}

/** Align samples onto a fixed grid of `bucketMs` rows, averaging each series per bucket. */
function buildBucketedRows(
  metrics: MetricPoint[],
  labelOf: (m: MetricPoint) => string,
  bucketMs: number,
  rangeMs?: { from: number; to: number },
): SeriesPoint[] {
  if (bucketMs <= 0) return []

  const values = new Map<number, Map<string, number[]>>()
  let minBucket = rangeMs?.from != null ? Math.floor(rangeMs.from / bucketMs) * bucketMs : Infinity
  let maxBucket = rangeMs?.to != null ? Math.floor(rangeMs.to / bucketMs) * bucketMs : -Infinity

  for (const m of metrics) {
    const bucket = Math.floor(m.time_ms / bucketMs) * bucketMs
    if (bucket < minBucket) minBucket = bucket
    if (bucket > maxBucket) maxBucket = bucket
    const label = labelOf(m)
    let byLabel = values.get(bucket)
    if (!byLabel) {
      byLabel = new Map()
      values.set(bucket, byLabel)
    }
    const list = byLabel.get(label) ?? []
    list.push(m.value)
    byLabel.set(label, list)
  }

  if (!Number.isFinite(minBucket) || !Number.isFinite(maxBucket)) return []

  const rows: SeriesPoint[] = []
  for (let b = minBucket; b <= maxBucket; b += bucketMs) {
    const row: SeriesPoint = { time: new Date(b).toLocaleTimeString(), time_ms: b }
    const byLabel = values.get(b)
    if (byLabel) {
      for (const [label, list] of byLabel) {
        const sum = list.reduce((acc, v) => acc + v, 0)
        row[label] = Math.round((sum / list.length) * 1000) / 1000
      }
    }
    rows.push(row)
  }
  return rows
}

/** Group successful metrics into per (agent, target) series, one aligned row per time bucket. */
export function buildChartData(metrics: MetricPoint[], options: SeriesOptions = {}): SeriesPoint[] {
  const { bucketMs = DEFAULT_CADENCE_MS, includeCheckType = false, rangeMs } = options
  return buildBucketedRows(
    metrics.filter((m) => m.success),
    (m) => seriesLabel(m, includeCheckType),
    bucketMs,
    rangeMs,
  )
}

/** Unique series labels in first-seen order (mirrors the keys used by buildChartData). */
export function buildChartLines(
  metrics: MetricPoint[],
  options: Pick<SeriesOptions, 'includeCheckType'> = {},
): string[] {
  const { includeCheckType = false } = options
  const seen = new Set<string>()
  const lines: string[] = []
  for (const m of metrics) {
    if (!m.success) continue
    const label = seriesLabel(m, includeCheckType)
    if (!seen.has(label)) {
      seen.add(label)
      lines.push(label)
    }
  }
  return lines
}

/** Row keys used for the probe-outcome bars behind the line chart. */
export const OUTCOME_OK_KEY = '__ok'
export const OUTCOME_FAIL_KEY = '__fail'

/** Successful/failed probe counts per time bucket, aligned to the same grid as buildChartData. */
export function buildOutcomeRows(
  metrics: MetricPoint[],
  options: SeriesOptions = {},
): SeriesPoint[] {
  const { bucketMs = DEFAULT_CADENCE_MS, rangeMs } = options
  if (bucketMs <= 0) return []

  let minBucket = rangeMs?.from != null ? Math.floor(rangeMs.from / bucketMs) * bucketMs : Infinity
  let maxBucket = rangeMs?.to != null ? Math.floor(rangeMs.to / bucketMs) * bucketMs : -Infinity
  const totals = new Map<number, { ok: number; fail: number }>()
  for (const m of metrics) {
    const bucket = Math.floor(m.time_ms / bucketMs) * bucketMs
    if (bucket < minBucket) minBucket = bucket
    if (bucket > maxBucket) maxBucket = bucket
    const acc = totals.get(bucket) ?? { ok: 0, fail: 0 }
    if (m.success) acc.ok += 1
    else acc.fail += 1
    totals.set(bucket, acc)
  }

  if (!Number.isFinite(minBucket) || !Number.isFinite(maxBucket)) return []

  const rows: SeriesPoint[] = []
  for (let b = minBucket; b <= maxBucket; b += bucketMs) {
    const acc = totals.get(b) ?? { ok: 0, fail: 0 }
    rows.push({
      time: new Date(b).toLocaleTimeString(),
      time_ms: b,
      [OUTCOME_OK_KEY]: acc.ok,
      [OUTCOME_FAIL_KEY]: acc.fail,
    })
  }
  return rows
}

export interface TlsCertInfo {
  target: string
  agent_id: string
  expiry_days: number | null
  subject: string
  issuer: string
  hostname_match: string
}

/** Latest successful tls_certificate metric per target. */
export function buildTlsCerts(metrics: MetricPoint[]): TlsCertInfo[] {
  const latest = new Map<string, MetricPoint>()
  metrics
    .filter((m) => m.check_type === 'tls_certificate' && m.success)
    .forEach((m) => latest.set(m.target, m))
  return Array.from(latest.entries()).map(([target, m]) => {
    const days = Number(m.attributes?.tls_cert_expiry_days ?? NaN)
    return {
      target,
      agent_id: m.agent_id,
      expiry_days: Number.isFinite(days) ? days : null,
      subject: m.attributes?.tls_cert_subject ?? '',
      issuer: m.attributes?.tls_cert_issuer ?? '',
      hostname_match: m.attributes?.tls_cert_hostname_match ?? 'unknown',
    }
  })
}

/** NTP offset time series, one line per agent, aligned into shared time buckets. */
export function buildNtpSeries(metrics: MetricPoint[], options: SeriesOptions = {}): SeriesPoint[] {
  const { bucketMs = DEFAULT_CADENCE_MS, rangeMs } = options
  return buildBucketedRows(
    metrics.filter((m) => m.check_type === 'ntp_offset' && m.success),
    (m) => m.agent_id,
    bucketMs,
    rangeMs,
  )
}

export function buildNtpAgents(metrics: MetricPoint[]): string[] {
  const keys = new Set<string>()
  metrics
    .filter((m) => m.check_type === 'ntp_offset' && m.success)
    .forEach((m) => keys.add(m.agent_id))
  return Array.from(keys)
}
