import { Menu, Moon, Sun } from 'lucide-react'
import { cn } from '../../lib/cn'
import { useDashboard } from '../../context/DashboardContext'
import { useHealth } from '../../hooks/useHealth'
import { useTheme } from '../../hooks/useTheme'
import { VIEW_LABELS } from '../../lib/constants'
import { LastUpdated } from '../ui/LastUpdated'

interface HeaderProps {
  lastUpdated: number | null
  onOpenSidebar: () => void
}

function HealthPill() {
  const { data: health, error, isLoading } = useHealth()

  let tone: 'ok' | 'degraded' | 'down' | 'loading' = 'loading'
  if (error) tone = 'down'
  else if (health?.status === 'ok') tone = 'ok'
  else if (health?.status === 'degraded') tone = 'degraded'

  const label =
    error != null
      ? 'Disconnected'
      : health?.status === 'ok'
        ? 'Collector OK'
        : health?.status === 'degraded'
          ? `Degraded${health?.storage ? '' : ' · storage down'}`
          : 'Connecting…'

  return (
    <span
      role="status"
      title={
        error != null
          ? `Disconnected: ${error instanceof Error ? error.message : 'network error'}`
          : `Collector status: ${health?.status ?? 'unknown'}`
      }
      className="inline-flex items-center gap-2 rounded-full border border-border bg-surface px-3 py-1"
    >
      <span className="relative flex size-2">
        {tone === 'ok' && (
          <span className="absolute inline-flex size-full animate-ping rounded-full bg-success opacity-60" />
        )}
        <span
          className={cn(
            'relative inline-flex size-2 rounded-full',
            tone === 'ok' && 'bg-success',
            tone === 'degraded' && 'bg-warning',
            tone === 'down' && 'bg-critical',
            tone === 'loading' && 'bg-warning animate-pulse',
          )}
        />
      </span>
      <span className="text-xs font-medium text-fg-muted">{label}</span>
      {!isLoading && health?.storage != null && (
        <span className="text-[10px] text-fg-subtle">storage {health.storage ? 'ok' : 'down'}</span>
      )}
    </span>
  )
}

export function Header({ lastUpdated, onOpenSidebar }: HeaderProps) {
  const { theme, toggleTheme } = useTheme()
  const { view } = useDashboard()
  const dark = theme === 'dark'

  return (
    <header className="sticky top-0 z-30 flex h-14 shrink-0 items-center gap-3 border-b border-border bg-surface/90 px-4 backdrop-blur">
      <button
        onClick={onOpenSidebar}
        aria-label="Open navigation"
        className="rounded-md p-2 text-fg-muted hover:bg-surface-muted hover:text-fg lg:hidden"
      >
        <Menu className="size-5" aria-hidden="true" />
      </button>

      <div className="flex min-w-0 flex-1 items-center gap-3 overflow-x-auto">
        <HealthPill />
        <span className="hidden text-sm font-medium text-fg sm:inline" aria-hidden="true">
          {VIEW_LABELS[view] ?? 'Overview'}
        </span>
        <span className="hidden text-fg-subtle sm:inline" aria-hidden="true">·</span>
        <LastUpdated timestamp={lastUpdated} />
      </div>

      <button
        onClick={toggleTheme}
        aria-label={dark ? 'Switch to light mode' : 'Switch to dark mode'}
        className="rounded-md p-2 text-fg-muted transition-colors hover:bg-surface-muted hover:text-fg focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40"
      >
        {dark ? <Sun className="size-4" aria-hidden="true" /> : <Moon className="size-4" aria-hidden="true" />}
      </button>
    </header>
  )
}
