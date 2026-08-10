import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { HealthResponse } from '../types'

export function useHealth() {
  return useQuery({
    queryKey: ['health'],
    queryFn: () => apiGet<HealthResponse>('/api/health'),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}
