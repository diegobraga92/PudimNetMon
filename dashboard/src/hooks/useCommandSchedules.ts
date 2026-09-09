import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import type {
  CommandRunsResponse,
  CommandSchedulesResponse,
  ScheduleInput,
  ScheduleMutationResponse,
} from '../types'

/** Polls the persisted heavy-command schedules. */
export function useCommandSchedules() {
  return useQuery({
    queryKey: ['command-schedules'],
    queryFn: () => apiGet<CommandSchedulesResponse>('/api/command-schedules'),
    refetchInterval: 10_000,
    retry: 1,
  })
}

/** Recent command executions (scheduled + ad-hoc), newest first. */
export function useCommandRuns(limit = 50) {
  return useQuery({
    queryKey: ['command-runs', limit],
    queryFn: () =>
      apiGet<CommandRunsResponse>('/api/command-runs', { limit }),
    refetchInterval: 15_000,
    retry: 1,
  })
}

/** Invalidate both the schedule list and the run history cache. */
function useRefreshSchedules() {
  const queryClient = useQueryClient()
  return () => {
    void queryClient.invalidateQueries({ queryKey: ['command-schedules'] })
    void queryClient.invalidateQueries({ queryKey: ['command-runs'] })
  }
}

export function useCreateSchedule() {
  const refresh = useRefreshSchedules()
  return useMutation({
    mutationFn: (input: ScheduleInput): Promise<ScheduleMutationResponse> =>
      apiPost('/api/command-schedules', input),
    onSuccess: refresh,
  })
}

export function useSetScheduleEnabled() {
  const refresh = useRefreshSchedules()
  return useMutation({
    mutationFn: (input: {
      id: string
      enabled: boolean
    }): Promise<ScheduleMutationResponse> =>
      apiPost('/api/command-schedules/enable', input),
    onSuccess: refresh,
  })
}

export function useDeleteSchedule() {
  const refresh = useRefreshSchedules()
  return useMutation({
    mutationFn: (id: string): Promise<ScheduleMutationResponse> =>
      apiPost('/api/command-schedules/delete', { id }),
    onSuccess: refresh,
  })
}
