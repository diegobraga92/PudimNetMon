import { ShieldCheck } from 'lucide-react'
import { useMetrics } from '../../hooks/useMetrics'
import { buildTlsCerts } from '../../lib/derive'
import { useMemo } from 'react'
import { useDashboard } from '../../context/DashboardContext'
import { cn } from '../../lib/cn'
import { ChartContainer } from './ChartContainer'
import { Card } from '../ui/Card'

function certTone(days: number | null): 'ok' | 'warn' | 'critical' | 'unknown' {
  if (days === null) return 'unknown'
  if (days < 0) return 'critical'
  if (days < 7) return 'critical'
  if (days < 30) return 'warn'
  return 'ok'
}

const toneStyles = {
  ok: 'border-l-success',
  warn: 'border-l-warning',
  critical: 'border-l-critical',
  unknown: 'border-l-border-strong',
}

const toneText = {
  ok: 'text-success',
  warn: 'text-warning',
  critical: 'text-critical',
  unknown: 'text-fg-muted',
}

export function TlsCertsGrid() {
  const { selectedAgent } = useDashboard()
  const { data, isLoading } = useMetrics({
    agentId: selectedAgent,
    checkType: 'tls_certificate',
    windowSeconds: 3600,
  })
  const certs = useMemo(() => buildTlsCerts(data ?? []), [data])

  return (
    <ChartContainer
      title="TLS Certificate Expiry"
      subtitle="Days remaining per monitored certificate"
      isLoading={isLoading}
      isEmpty={certs.length === 0}
      emptyMessage="No TLS certificate data yet. Configure TLS targets on an agent."
      emptyIcon={<ShieldCheck className="size-8" aria-hidden="true" />}
    >
      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
        {certs.map((cert) => {
          const tone = certTone(cert.expiry_days)
          const days = cert.expiry_days
          return (
            <Card key={cert.target} className={cn('border-l-4 p-4', toneStyles[tone])}>
              <div className="flex items-center justify-between gap-2">
                <strong className="truncate text-sm text-fg" title={cert.target}>
                  {cert.target}
                </strong>
                <span className={cn('shrink-0 text-xs font-semibold', toneText[tone])}>
                  {days === null ? 'n/a' : days < 0 ? `${Math.abs(days)}d EXPIRED` : `${days}d left`}
                </span>
              </div>
              <p className="mt-1 truncate text-xs text-fg-muted" title={cert.subject}>
                {cert.agent_id} · {cert.subject}
              </p>
              <p className="mt-0.5 truncate text-xs text-fg-subtle" title={cert.issuer}>
                issuer: {cert.issuer}
              </p>
              <p className="mt-0.5 text-xs text-fg-subtle">hostname match: {cert.hostname_match}</p>
            </Card>
          )
        })}
      </div>
    </ChartContainer>
  )
}
