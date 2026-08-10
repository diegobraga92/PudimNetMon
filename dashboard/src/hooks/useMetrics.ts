import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { MetricPoint } from '../types'

export interface MetricsParams {
  agentId: string
  checkType: string
  windowSeconds: number
}

export function useMetrics({ agentId, checkType, windowSeconds }: MetricsParams) {
  return useQuery({
    queryKey: ['metrics', agentId, checkType, windowSeconds],
    queryFn: () =>
      apiGet<MetricPoint[]>('/api/metrics', {
        agent_id: agentId === 'all' ? undefined : agentId,
        check_type: checkType === 'all' ? undefined : checkType,
        window_seconds: windowSeconds,
      }),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}
