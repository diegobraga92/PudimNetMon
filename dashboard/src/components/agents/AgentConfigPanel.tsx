import { useState } from 'react'
import { Loader2, RefreshCcw, Settings2, Zap } from 'lucide-react'
import type { AgentConfigForm } from '../../types'
import { useAgents } from '../../hooks/useAgents'
import {
  DEFAULT_CONFIG_FORM,
  parseApplied,
  useApplyAgentConfig,
  useLoadAgentConfig,
} from '../../hooks/useAgentConfig'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { Input } from '../ui/Input'
import { Select } from '../ui/Select'
import { Switch } from '../ui/Switch'
import { EmptyState } from '../ui/EmptyState'
import { useToast } from '../ui/toast'

type FormErrors = Partial<Record<keyof AgentConfigForm, string>>

const HOSTNAME_RE = /^[a-zA-Z0-9.-]+$/
const HOSTPORT_RE = /^[a-zA-Z0-9.-]+:\d+$/
const URL_RE = /^https?:\/\/.+/

function validateForm(form: AgentConfigForm): FormErrors {
  const errors: FormErrors = {}
  const checkList = (
    field: keyof AgentConfigForm,
    value: string,
    validator: (entry: string) => boolean,
    label: string,
  ) => {
    if (!value.trim()) return
    const entries = value.split(',').map((s) => s.trim()).filter(Boolean)
    if (entries.some((e) => !validator(e))) {
      errors[field] = `Invalid ${label} — check each comma-separated entry`
    }
  }
  checkList('dns', form.dns, (e) => HOSTNAME_RE.test(e), 'hostname')
  checkList('tcp', form.tcp, (e) => HOSTPORT_RE.test(e), 'host:port')
  checkList('tls', form.tls, (e) => HOSTPORT_RE.test(e), 'host:port')
  checkList('http', form.http, (e) => URL_RE.test(e), 'URL')
  checkList('ping', form.ping, (e) => HOSTNAME_RE.test(e) || URL_RE.test(e), 'hostname/URL')
  checkList('httpProtocols', form.httpProtocols, (e) => /^http(1\.1|2|3)$/.test(e), 'protocol')

  const count = Number(form.pingCount)
  if (!Number.isInteger(count) || count < 1 || count > 100) {
    errors.pingCount = 'Ping count must be 1–100'
  }
  return errors
}

export function AgentConfigPanel() {
  const agentsQuery = useAgents()
  const loadConfig = useLoadAgentConfig()
  const applyConfig = useApplyAgentConfig()

  const [configAgent, setConfigAgent] = useState('')
  const [form, setForm] = useState<AgentConfigForm>(DEFAULT_CONFIG_FORM)
  const [errors, setErrors] = useState<FormErrors>({})
  const [result, setResult] = useState<{ success: boolean; message: string } | null>(null)
  const { toast } = useToast()

  const agents = agentsQuery.data?.agents ?? []

  const selectAgent = (agentId: string) => {
    setConfigAgent(agentId)
    setResult(null)
    setErrors({})
    if (!agentId) {
      setForm(DEFAULT_CONFIG_FORM)
      return
    }
    loadConfig.mutate(agentId, {
      onSuccess: (data) => {
        if (data.success) {
          setForm(parseApplied(data.applied))
          setResult({ success: true, message: 'Current configuration loaded.' })
        } else {
          setResult({ success: false, message: data.error || 'Failed to load configuration' })
          toast({ title: 'Failed to load config', description: data.error, variant: 'error' })
        }
      },
      onError: (err) => {
        setResult({ success: false, message: err instanceof Error ? err.message : 'Failed to load configuration' })
        toast({ title: 'Failed to load config', description: err instanceof Error ? err.message : 'Unknown error', variant: 'error' })
      },
    })
  }

  const setField = <K extends keyof AgentConfigForm>(key: K, value: AgentConfigForm[K]) => {
    setForm((prev) => ({ ...prev, [key]: value }))
    setErrors((prev) => ({ ...prev, [key]: undefined }))
  }

  const handleApply = () => {
    if (!configAgent) return
    const nextErrors = validateForm(form)
    setErrors(nextErrors)
    if (Object.keys(nextErrors).length > 0) return
    applyConfig.mutate(
      { agentId: configAgent, form },
      {
        onSuccess: (data) => {
          if (data.success) {
            setResult({ success: true, message: `Applied — ${data.applied}` })
            toast({ title: 'Configuration applied', description: data.applied, variant: 'success' })
          } else {
            setResult({ success: false, message: data.error || data.applied || 'Failed to apply' })
            toast({ title: 'Failed to apply', description: data.error || data.applied, variant: 'error' })
          }
        },
        onError: (err) => {
          setResult({ success: false, message: err instanceof Error ? err.message : 'Failed to apply' })
          toast({ title: 'Failed to apply', description: err instanceof Error ? err.message : 'Unknown error', variant: 'error' })
        },
      },
    )
  }

  const agentOptions = [
    { value: '', label: 'Select agent…' },
    ...agents.map((a) => ({ value: a.agent_id, label: `${a.agent_id}${a.alive ? ' (alive)' : ' (offline)'}` })),
  ]

  const loading = loadConfig.isPending || applyConfig.isPending
  return (
    <Card className="p-5">
      <div className="mb-4 flex flex-wrap items-center justify-between gap-2">
        <div>
          <h2 className="text-sm font-semibold text-fg">Probe Targets</h2>
          <p className="mt-0.5 text-xs text-fg-muted">
            Edit probe targets for one agent. Changes apply on the next probe cycle — no restart required.
          </p>
        </div>
        <Settings2 className="size-5 text-fg-subtle" aria-hidden="true" />
      </div>

      {agents.length === 0 ? (
        <EmptyState
          icon={<Zap className="size-7" aria-hidden="true" />}
          title="No agents to configure"
          description="Register an agent first, then reload this page."
        />
      ) : (
        <>
          <div className="mb-4 flex flex-wrap items-center gap-2">
            <div className="w-full max-w-xs">
              <Select
                ariaLabel="Select agent to configure"
                value={configAgent}
                onValueChange={selectAgent}
                options={agentOptions}
                placeholder="Select agent…"
              />
            </div>
            {configAgent && (
              <Button
                variant="ghost"
                size="sm"
                disabled={loading}
                onClick={() => selectAgent(configAgent)}
              >
                <RefreshCcw className="size-3.5" aria-hidden="true" />
                Reload
              </Button>
            )}
          </div>

          {result && (
            <p
              role="status"
              className={`mb-4 rounded-lg border px-3 py-2 text-xs ${
                result.success
                  ? 'border-success/40 bg-success/10 text-success'
                  : 'border-critical/40 bg-critical/10 text-critical'
              }`}
            >
              {result.message}
            </p>
          )}

          {configAgent ? (
            <>
              <div className="grid gap-4 sm:grid-cols-2">
                <Input
                  label="DNS targets"
                  value={form.dns}
                  onChange={(e) => setField('dns', e.target.value)}
                  placeholder="example.com,cloudflare.com"
                  hint="Comma-separated hostnames"
                  error={errors.dns}
                />
                <Input
                  label="TCP targets (host:port)"
                  value={form.tcp}
                  onChange={(e) => setField('tcp', e.target.value)}
                  placeholder="example.com:443"
                  error={errors.tcp}
                />
                <Input
                  label="TLS targets (host:port)"
                  value={form.tls}
                  onChange={(e) => setField('tls', e.target.value)}
                  placeholder="example.com:443"
                  error={errors.tls}
                />
                <Input
                  label="HTTP targets"
                  value={form.http}
                  onChange={(e) => setField('http', e.target.value)}
                  placeholder="https://example.com"
                  error={errors.http}
                />
                <Input
                  label="Ping targets"
                  value={form.ping}
                  onChange={(e) => setField('ping', e.target.value)}
                  placeholder="example.com"
                  error={errors.ping}
                />
                <Input
                  label="Ping count"
                  type="number"
                  min={1}
                  max={100}
                  value={form.pingCount}
                  onChange={(e) => setField('pingCount', e.target.value)}
                  error={errors.pingCount}
                />
                <Input
                  label="HTTP protocols"
                  value={form.httpProtocols}
                  onChange={(e) => setField('httpProtocols', e.target.value)}
                  placeholder="http1.1,http2,http3"
                  hint="Comma-separated: http1.1, http2, http3"
                  error={errors.httpProtocols}
                />
              </div>

              <div className="mt-4 flex flex-wrap gap-x-6 gap-y-3">
                <Switch
                  label="TLS cert validation"
                  checked={form.tlsCert}
                  onCheckedChange={(v) => setField('tlsCert', v)}
                />
                <Switch
                  label="TCP retransmit probe"
                  checked={form.tcpRetransmit}
                  onCheckedChange={(v) => setField('tcpRetransmit', v)}
                />
                <Switch
                  label="TCP handshake capture (pcap)"
                  checked={form.tcpHandshake}
                  onCheckedChange={(v) => setField('tcpHandshake', v)}
                />
              </div>

              <div className="mt-6 flex flex-wrap items-center gap-3">
                <Button onClick={handleApply} loading={applyConfig.isPending} disabled={loading}>
                  <Zap className="size-4" aria-hidden="true" />
                  Apply configuration
                </Button>
                <Button
                  variant="ghost"
                  size="sm"
                  disabled={loading}
                  onClick={() => {
                    setForm(DEFAULT_CONFIG_FORM)
                    setErrors({})
                  }}
                >
                  Reset to defaults
                </Button>
              </div>
            </>
          ) : (
            <p className="text-sm text-fg-subtle">Select an agent to load and edit its configuration.</p>
          )}

          {loadConfig.isPending && (
            <p className="mt-3 flex items-center gap-2 text-xs text-fg-muted">
              <Loader2 className="size-3.5 animate-spin" aria-hidden="true" />
              Loading current configuration…
            </p>
          )}
        </>
      )}
    </Card>
  )
}

