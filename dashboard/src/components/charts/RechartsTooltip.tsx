import type { ReactNode } from 'react'
import { OUTCOME_FAIL_KEY, OUTCOME_OK_KEY } from '../../lib/derive'
import { formatDateTime } from '../../lib/formatters'

interface TooltipRow {
  dataKey: string
  name?: string
  value?: number | string
  color?: string
  unit?: string
  /** Original datum row (Recharts attaches this automatically). */
  payload?: { time_ms?: number; [key: string]: unknown }
}

interface RechartsTooltipProps {
  active?: boolean
  payload?: TooltipRow[]
  label?: string | number
  labelFormatter?: (label: string | number) => ReactNode
  /** Default unit appended to values that don't specify their own. */
  unit?: string
  /** Show the full date+time for hovered rows (used on long time windows). */
  showDate?: boolean
}

/** Format probe values compactly (2 decimals max, thousands separators). */
function formatValue(value: number | string | undefined): string {
  if (typeof value !== 'number') return value != null ? String(value) : '—'
  if (Number.isNaN(value)) return '—'
  return value.toLocaleString(undefined, { maximumFractionDigits: 2 })
}

/** Shared themed tooltip for Recharts. */
export function RechartsTooltip({ active, payload, label, labelFormatter, unit, showDate }: RechartsTooltipProps) {
  if (!active || !payload || payload.length === 0) return null

  const firstDatum = payload[0]?.payload
  let title: ReactNode = label != null ? (labelFormatter ? labelFormatter(label) : label) : null
  if (showDate && typeof firstDatum?.time_ms === 'number') {
    title = formatDateTime(firstDatum.time_ms)
  }

  return (
    <div className="rounded-lg border border-border bg-surface-raised px-3 py-2 text-xs shadow-xl">
      {title != null && <p className="mb-1 font-semibold text-fg">{title}</p>}
      {payload.map((row) => {
        const isOutcomeRow = row.dataKey === OUTCOME_FAIL_KEY || row.dataKey === OUTCOME_OK_KEY
        const rowUnit = isOutcomeRow ? '' : row.unit ?? unit ?? ''
        const suffix =
          row.dataKey === OUTCOME_FAIL_KEY ? ' failed' : row.dataKey === OUTCOME_OK_KEY ? ' ok' : rowUnit
        return (
          <p key={row.dataKey} className="flex items-center gap-2 py-0.5 text-fg-muted">
            <span
              className="inline-block size-2 rounded-full"
              style={{ backgroundColor: row.color }}
              aria-hidden="true"
            />
            <span className="truncate">{row.name ?? row.dataKey}</span>
            <span className="ml-auto font-medium text-fg">
              {row.value != null ? `${formatValue(row.value)}${suffix}` : '—'}
            </span>
          </p>
        )
      })}
    </div>
  )
}
