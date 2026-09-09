import { RotateCcw } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useAgents } from '../../hooks/useAgents'
import {
  CHECK_TYPE_OPTIONS,
  DEFAULT_CHECK_FILTER,
  DEFAULT_WINDOW_SECONDS,
  TIME_WINDOWS,
} from '../../lib/constants'
import { cn } from '../../lib/cn'
import { Button } from './Button'

const WINDOW_SHORT_LABELS: Record<number, string> = {
  60: '1m',
  300: '5m',
  900: '15m',
  3600: '1h',
  21600: '6h',
  86400: '24h',
}

/** "DNS Resolution (ms)" -> "DNS Resolution" for compact chips. */
function shortCheckLabel(label: string): string {
  return label.replace(/\s*\(ms\)$/, '')
}

interface ChipProps {
  active: boolean
  label: string
  title?: string
  onClick: () => void
}

function Chip({ active, label, title, onClick }: ChipProps) {
  return (
    <button
      type="button"
      aria-pressed={active}
      title={title ?? label}
      onClick={onClick}
      className={cn(
        'inline-flex h-7 shrink-0 items-center rounded-full border px-2.5 text-xs font-medium whitespace-nowrap transition-colors',
        'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
        active
          ? 'border-accent/40 bg-accent/10 text-accent'
          : 'border-border bg-surface text-fg-muted hover:border-border-strong hover:text-fg',
      )}
    >
      {label}
    </button>
  )
}

interface FilterGroupProps {
  label: string
  options: { value: string; label: string; title?: string }[]
  value: string
  onSelect: (value: string) => void
}

function FilterGroup({ label, options, value, onSelect }: FilterGroupProps) {
  return (
    <div className="flex items-center gap-1.5" role="group" aria-label={`Filter by ${label.toLowerCase()}`}>
      <span className="w-10 shrink-0 text-[10px] font-semibold tracking-wider text-fg-subtle uppercase">{label}</span>
      <div className="flex max-w-full flex-wrap items-center gap-1.5">
        {options.map((option) => (
          <Chip
            key={option.value}
            active={value === option.value}
            label={option.label}
            title={option.title}
            onClick={() => onSelect(option.value)}
          />
        ))}
      </div>
    </div>
  )
}

/** Shared quick-filter toolbar backed by dashboard state so all views stay in sync. */
export function FilterBar({ className }: { className?: string }) {
  const {
    selectedAgent,
    setSelectedAgent,
    selectedCheck,
    setSelectedCheck,
    windowSeconds,
    setWindowSeconds,
  } = useDashboard()
  const agentsQuery = useAgents()

  const agents = agentsQuery.data?.agents ?? []
  const agentOptions = [
    { value: 'all', label: 'All' },
    ...agents.map((a) => ({ value: a.agent_id, label: a.agent_id })),
  ]
  const checkOptions = CHECK_TYPE_OPTIONS.map((o) => ({
    value: o.value,
    label: o.value === 'all' ? 'All' : shortCheckLabel(o.label),
    title: o.label,
  }))
  const windowOptions = TIME_WINDOWS.map((w) => ({
    value: String(w.value),
    label: WINDOW_SHORT_LABELS[w.value] ?? w.label,
  }))

  const hasActiveFilters =
    selectedAgent !== 'all' || selectedCheck !== DEFAULT_CHECK_FILTER || windowSeconds !== DEFAULT_WINDOW_SECONDS

  const reset = () => {
    setSelectedAgent('all')
    setSelectedCheck(DEFAULT_CHECK_FILTER)
    setWindowSeconds(DEFAULT_WINDOW_SECONDS)
  }

  return (
    <div className={cn('flex flex-col gap-2', className)}>
      <FilterGroup
        label="Agent"
        options={agentOptions}
        value={selectedAgent}
        onSelect={(v) => setSelectedAgent(v === 'all' ? 'all' : v)}
      />
      <FilterGroup
        label="Check"
        options={checkOptions}
        value={selectedCheck}
        onSelect={(v) => setSelectedCheck(v as typeof selectedCheck)}
      />
      <div className="flex flex-wrap items-center justify-between gap-2">
        <FilterGroup
          label="Window"
          options={windowOptions}
          value={String(windowSeconds)}
          onSelect={(v) => setWindowSeconds(Number(v))}
        />
        {hasActiveFilters && (
          <Button variant="ghost" size="sm" onClick={reset} aria-label="Reset filters">
            <RotateCcw className="size-3.5" aria-hidden="true" />
            Reset
          </Button>
        )}
      </div>
    </div>
  )
}
