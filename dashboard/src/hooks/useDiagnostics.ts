import { useMutation } from '@tanstack/react-query'
import { apiPostForm } from '../lib/api'
import type { AgentInfo, DiagnosticResult } from '../types'

const DEFAULT_TRACE_TARGET = 'example.com'
const DEFAULT_PCAP_DURATION_S = '5'
const DEFAULT_PCAP_FILTER = 'tcp port 443'

export function useRunDiagnostic() {
  return useMutation({
    mutationFn: async (agent: AgentInfo): Promise<DiagnosticResult> => {
      const data = await apiPostForm<DiagnosticResult>('/api/diagnostic', {
        agent_id: agent.agent_id,
        trace_target: DEFAULT_TRACE_TARGET,
        pcap_duration_s: DEFAULT_PCAP_DURATION_S,
        pcap_filter: DEFAULT_PCAP_FILTER,
      })
      return data
    },
  })
}
