import { useMutation } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import type { AgentConfigForm, AgentConfigResponse } from '../types'

/** Parse the `applied="key=value key=value"` server payload into a form shape. */
export function parseApplied(applied: string): AgentConfigForm {
  const fields: Record<string, string> = {}
  applied.split(/\s+/).forEach((pair) => {
    const eq = pair.indexOf('=')
    if (eq > 0) fields[pair.slice(0, eq)] = pair.slice(eq + 1)
  })
  const setField = (key: string) => fields[key] ?? ''
  return {
    dns: setField('dns'),
    tcp: setField('tcp'),
    tls: setField('tls'),
    http: setField('http'),
    ping: setField('ping'),
    pingCount: setField('ping_count') || '4',
    pingGap: setField('ping_gap_ms') || '200',
    tlsCert: setField('tls_cert') !== 'off',
    tcpRetransmit: setField('tcp_retransmit') !== 'off',
    tcpHandshake: setField('tcp_handshake') !== 'off',
    httpProtocols: setField('http_protocols'),
  }
}

export const DEFAULT_CONFIG_FORM: AgentConfigForm = {
  dns: '',
  tcp: '',
  tls: '',
  http: '',
  ping: '',
  pingCount: '4',
  pingGap: '200',
  tlsCert: true,
  tcpRetransmit: true,
  tcpHandshake: true,
  httpProtocols: '',
}

export function useLoadAgentConfig() {
  return useMutation({
    mutationFn: async (agentId: string): Promise<AgentConfigResponse> => {
      const data = await apiGet<AgentConfigResponse>(
        `/api/agents/config?agent_id=${encodeURIComponent(agentId)}`,
      )
      return data
    },
  })
}

export function useApplyAgentConfig() {
  return useMutation({
    mutationFn: async ({
      agentId,
      form,
    }: {
      agentId: string
      form: AgentConfigForm
    }): Promise<AgentConfigResponse> => {
      const split = (s: string) => s.split(',').map((x) => x.trim()).filter((x) => x.length > 0)
      return apiPost<AgentConfigResponse>('/api/agents/config', {
        agent_id: agentId,
        dns_targets: split(form.dns),
        tcp_targets: split(form.tcp),
        tls_targets: split(form.tls),
        http_targets: split(form.http),
        ping_targets: split(form.ping),
        ping_count: Number(form.pingCount) || 4,
        ping_gap_ms: Number(form.pingGap) || 200,
        tls_cert_check: form.tlsCert,
        tcp_retransmit_check: form.tcpRetransmit,
        tcp_handshake_capture: form.tcpHandshake,
        http_protocols: split(form.httpProtocols),
      })
    },
  })
}
