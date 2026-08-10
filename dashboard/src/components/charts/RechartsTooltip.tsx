import type { ReactNode } from 'react'

interface TooltipRow {
  dataKey: string
  name?: string
  value?: number | string
  color?: string
  unit?: string
}

interface RechartsTooltipProps {
  active?: boolean
  payload?: TooltipRow[]
  label?: string | number
  labelFormatter?: (label: string | number) => ReactNode
  /** Default unit appended to values that don't specify their own. */
  unit?: string
}

/** Shared themed tooltip for Recharts. */
export function RechartsTooltip({ active, payload, label, labelFormatter, unit }: RechartsTooltipProps) {
  if (!active || !payload || payload.length === 0) return null
  return (
    <div className="rounded-lg border border-border bg-surface-raised px-3 py-2 text-xs shadow-xl">
      {label != null && (
        <p className="mb-1 font-semibold text-fg">
          {labelFormatter ? labelFormatter(label) : label}
        </p>
      )}
      {payload.map((row) => (
        <p key={row.dataKey} className="flex items-center gap-2 py-0.5 text-fg-muted">
          <span
            className="inline-block size-2 rounded-full"
            style={{ backgroundColor: row.color }}
            aria-hidden="true"
          />
          <span className="truncate">{row.name ?? row.dataKey}</span>
          <span className="ml-auto font-medium text-fg">
            {row.value != null ? `${row.value}${row.unit ?? unit ?? ''}` : '—'}
          </span>
        </p>
      ))}
    </div>
  )
}
