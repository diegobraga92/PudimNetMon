import { describe, expect, it } from 'vitest'
import type { MetricPoint } from '../types'
import {
  buildChartData,
  buildChartLines,
  buildNtpSeries,
  buildOutcomeRows,
  OUTCOME_FAIL_KEY,
  OUTCOME_OK_KEY,
  pickBucketMs,
} from './derive'

/** Shorthand for building a successful metric. */
function metric(partial: Partial<MetricPoint> & Pick<MetricPoint, 'time_ms'>): MetricPoint {
  return {
    agent_id: 'agent-1',
    check_type: 'icmp_ping',
    target: 'example.com',
    success: true,
    value: 10,
    ...partial,
  }
}

describe('pickBucketMs', () => {
  it('defaults to a 1s bucket for a short window with no data', () => {
    expect(pickBucketMs(300)).toBe(1000)
  })

  it('keeps a 24h window bounded to a few hundred buckets', () => {
    expect(pickBucketMs(86400)).toBe(300000)
  })

  it('widens the bucket to the probe cadence when it is coarser than the window average', () => {
    // One series sampled every 100s inside a 5 minute window.
    const metrics = [0, 100, 200, 300].map((s) => metric({ time_ms: s * 1000 }))
    expect(pickBucketMs(300, metrics)).toBe(120000)
  })
})

describe('buildChartData', () => {
  const BASE = 1_700_000_000_000

  it('aligns different series onto the same row when they share a bucket', () => {
    const rows = buildChartData(
      [
        metric({ time_ms: BASE, agent_id: 'agent-1', target: 'example.com', value: 12.4 }),
        metric({ time_ms: BASE + 400, agent_id: 'agent-3', target: 'example.com', value: 8.1 }),
      ],
      { bucketMs: 1000 },
    )
    expect(rows).toHaveLength(1)
    expect(rows[0].time_ms).toBe(Math.floor(BASE / 1000) * 1000)
    expect(rows[0]['agent-1 | example.com']).toBe(12.4)
    expect(rows[0]['agent-3 | example.com']).toBe(8.1)
  })

  it('averages multiple samples of the same series within one bucket', () => {
    const rows = buildChartData(
      [
        metric({ time_ms: BASE, value: 10 }),
        metric({ time_ms: BASE + 500, value: 20 }),
      ],
      { bucketMs: 1000 },
    )
    expect(rows).toHaveLength(1)
    expect(rows[0]['agent-1 | example.com']).toBe(15)
  })

  it('emits chronologically ordered rows and keeps empty buckets when a range is given', () => {
    const rows = buildChartData([metric({ time_ms: 3000, value: 5 })], {
      bucketMs: 1000,
      rangeMs: { from: 0, to: 5000 },
    })
    expect(rows.map((r) => r.time_ms)).toEqual([0, 1000, 2000, 3000, 4000, 5000])
    expect(rows[3]['agent-1 | example.com']).toBe(5)
    expect(rows[0]['agent-1 | example.com']).toBeUndefined()
  })

  it('drops failed probes', () => {
    const rows = buildChartData(
      [
        metric({ time_ms: BASE, value: 1 }),
        metric({ time_ms: BASE, value: 200, success: false }),
      ],
      { bucketMs: 1000 },
    )
    expect(rows[0]['agent-1 | example.com']).toBe(1)
  })
})

describe('series identity', () => {
  const metrics = [
    metric({ time_ms: 1000, check_type: 'icmp_ping', value: 15 }),
    metric({ time_ms: 1500, check_type: 'dns_resolution', value: 40 }),
  ]

  it('keeps different check types separate when includeCheckType is set', () => {
    const lines = buildChartLines(metrics, { includeCheckType: true })
    expect(lines).toEqual([
      'icmp_ping | agent-1 | example.com',
      'dns_resolution | agent-1 | example.com',
    ])
    const row = buildChartData(metrics, { bucketMs: 1000, includeCheckType: true })[0]
    expect(row['icmp_ping | agent-1 | example.com']).toBe(15)
    expect(row['dns_resolution | agent-1 | example.com']).toBe(40)
  })

  it('collapses to one merged series per agent + target by default', () => {
    expect(buildChartLines(metrics)).toEqual(['agent-1 | example.com'])
  })
})

describe('buildOutcomeRows', () => {
  const BASE = 1_700_000_000_000

  it('counts successful and failed probes per bucket', () => {
    const rows = buildOutcomeRows(
      [
        metric({ time_ms: BASE, success: true, value: 10 }),
        metric({ time_ms: BASE + 100, success: false, value: 200 }),
        metric({ time_ms: BASE + 200, success: false, value: 200 }),
      ],
      { bucketMs: 1000 },
    )
    expect(rows).toHaveLength(1)
    expect(rows[0][OUTCOME_OK_KEY]).toBe(1)
    expect(rows[0][OUTCOME_FAIL_KEY]).toBe(2)
  })

  it('aligns to the same grid as buildChartData across a range', () => {
    const metrics = [metric({ time_ms: 3000, success: false, value: 200 })]
    const rows = buildOutcomeRows(metrics, {
      bucketMs: 1000,
      rangeMs: { from: 0, to: 5000 },
    })
    expect(rows.map((r) => r.time_ms)).toEqual([0, 1000, 2000, 3000, 4000, 5000])
    expect(rows[3][OUTCOME_FAIL_KEY]).toBe(1)
    expect(rows[0][OUTCOME_FAIL_KEY]).toBe(0)
  })
})

describe('buildNtpSeries', () => {
  const BASE = 1_700_000_000_000

  it('keeps one line per agent and aligns samples into shared buckets', () => {
    const rows = buildNtpSeries(
      [
        metric({ time_ms: BASE, check_type: 'ntp_offset', agent_id: 'agent-1', value: 2.1 }),
        metric({ time_ms: BASE + 600, check_type: 'ntp_offset', agent_id: 'agent-3', value: -1.3 }),
      ],
      { bucketMs: 1000 },
    )
    expect(rows).toHaveLength(1)
    expect(rows[0]['agent-1']).toBe(2.1)
    expect(rows[0]['agent-3']).toBe(-1.3)
  })
})
