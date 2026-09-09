import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import { POLL_INTERVAL_MS } from '../lib/constants'
import type { AlertRule, AlertRulesResponse } from '../types'

export function useAlertRules() {
  return useQuery({
    queryKey: ['alert-rules'],
    queryFn: () => apiGet<AlertRulesResponse>('/api/alert-rules'),
    refetchInterval: POLL_INTERVAL_MS,
    retry: 1,
  })
}

/** Reflects the mutation response into the rules cache in one round trip. */
function useRefreshRules() {
  const queryClient = useQueryClient()
  return (data: AlertRulesResponse) => {
    if (data.success !== false) queryClient.setQueryData(['alert-rules'], data)
  }
}

export function useUpsertAlertRule() {
  const refresh = useRefreshRules()
  return useMutation({
    mutationFn: (rule: AlertRule) =>
      apiPost<AlertRulesResponse>('/api/alert-rules', { rule }),
    onSuccess: refresh,
  })
}

export function useDeleteAlertRule() {
  const refresh = useRefreshRules()
  return useMutation({
    mutationFn: (ruleId: string) =>
      apiPost<AlertRulesResponse>('/api/alert-rules/delete', { rule_id: ruleId }),
    onSuccess: refresh,
  })
}
