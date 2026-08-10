import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { AlertHistoryEntry } from '../types'

export function useAlertHistory(limit = 50) {
  return useQuery({
    queryKey: ['alert-history', limit],
    queryFn: () => apiGet<AlertHistoryEntry[]>(`/api/alert-history?limit=${limit}`),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}
