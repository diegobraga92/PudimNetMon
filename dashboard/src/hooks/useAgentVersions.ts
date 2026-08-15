import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import type { AgentVersionsResponse } from '../types'

/** Metadata for the self-hosted agent binaries the collector serves. */
export function useAgentVersions() {
  return useQuery({
    queryKey: ['agent-versions'],
    queryFn: () => apiGet<AgentVersionsResponse>('/api/agent/versions'),
    refetchInterval: 60_000,
    retry: 1,
  })
}
