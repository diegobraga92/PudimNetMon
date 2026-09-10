import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { AgentsDeleteResponse, AgentsResponse } from '../types'

export function useAgents() {
  return useQuery({
    queryKey: ['agents'],
    queryFn: () => apiGet<AgentsResponse>('/api/agents'),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}

/** Forgets one agent (e.g. one whose service was uninstalled). */
export function useDeleteAgent() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (agentId: string) =>
      apiPost<AgentsDeleteResponse>('/api/agents/delete', { agent_id: agentId }),
    onSuccess: (data) => {
      // The response already carries the updated registry snapshot.
      if (data.success !== false) queryClient.setQueryData(['agents'], { agents: data.agents })
    },
  })
}
