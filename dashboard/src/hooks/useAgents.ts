import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { AgentsResponse } from '../types'

export function useAgents() {
  return useQuery({
    queryKey: ['agents'],
    queryFn: () => apiGet<AgentsResponse>('/api/agents'),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}
