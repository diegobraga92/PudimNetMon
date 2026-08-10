import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { ActiveAlert } from '../types'

export function useAlerts() {
  return useQuery({
    queryKey: ['alerts'],
    queryFn: () => apiGet<ActiveAlert[]>('/api/alerts'),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}

export function useAckAlert() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: (alert: ActiveAlert) =>
      apiPost<{ alerts: ActiveAlert[] }>('/api/alerts/ack', {
        rule_id: alert.rule_id,
        agent_id: alert.agent_id,
        target: alert.target,
      }),
    onSuccess: (data) => {
      queryClient.setQueryData(['alerts'], data.alerts ?? [])
    },
  })
}

export function useAckAllAlerts() {
  const queryClient = useQueryClient()
  return useMutation({
    mutationFn: async (alerts: ActiveAlert[]) => {
      let latest: ActiveAlert[] = alerts
      for (const alert of alerts) {
        if (alert.acknowledged) continue
        const data = await apiPost<{ alerts: ActiveAlert[] }>('/api/alerts/ack', {
          rule_id: alert.rule_id,
          agent_id: alert.agent_id,
          target: alert.target,
        })
        latest = data.alerts ?? latest
      }
      return latest
    },
    onSuccess: (data) => {
      queryClient.setQueryData(['alerts'], data)
    },
  })
}
