import {
  Activity,
  Bell,
  History,
  LayoutDashboard,
  Radio,
  Settings2,
  Table2,
  X,
} from 'lucide-react'
import { cn } from '../../lib/cn'
import { useDashboard, type View } from '../../context/DashboardContext'
import { useAlerts } from '../../hooks/useAlerts'

const NAV_ITEMS: { view: View; label: string; icon: typeof LayoutDashboard }[] = [
  { view: 'overview', label: 'Overview', icon: LayoutDashboard },
  { view: 'metrics', label: 'Metrics', icon: Table2 },
  { view: 'agents', label: 'Agents', icon: Radio },
  { view: 'alerts', label: 'Alerts', icon: Bell },
  { view: 'history', label: 'Alert History', icon: History },
  { view: 'config', label: 'Agent Config', icon: Settings2 },
]

interface SidebarProps {
  open: boolean
  onClose: () => void
}

export function Sidebar({ open, onClose }: SidebarProps) {
  const { view, setView } = useDashboard()
  const { data: alerts } = useAlerts()
  const firingCount = alerts?.filter((a) => !a.acknowledged).length ?? 0

  const nav = (
    <nav className="flex flex-col gap-1 p-3" aria-label="Main navigation">
      {NAV_ITEMS.map((item) => {
        const Icon = item.icon
        const active = view === item.view
        return (
          <button
            key={item.view}
            onClick={() => {
              setView(item.view)
              onClose()
            }}
            aria-current={active ? 'page' : undefined}
            className={cn(
              'flex h-9 items-center gap-2.5 rounded-lg px-3 text-sm font-medium transition-colors',
              'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
              active
                ? 'bg-accent/10 text-accent'
                : 'text-fg-muted hover:bg-surface-muted hover:text-fg',
            )}
          >
            <Icon className="size-4 shrink-0" aria-hidden="true" />
            <span className="flex-1 text-left">{item.label}</span>
            {item.view === 'alerts' && firingCount > 0 && (
              <span className="inline-flex h-5 min-w-5 items-center justify-center rounded-full bg-critical px-1.5 text-[11px] font-semibold text-white">
                {firingCount}
              </span>
            )}
          </button>
        )
      })}
    </nav>
  )

  return (
    <>
      {/* Desktop sidebar */}
      <aside className="hidden w-60 shrink-0 flex-col border-r border-border bg-surface lg:flex">
        <div className="flex h-14 items-center gap-2 border-b border-border px-4">
          <Activity className="size-5 text-accent" aria-hidden="true" />
          <span className="text-sm font-bold tracking-tight text-fg">PudimNetMon</span>
        </div>
        {nav}
        <div className="mt-auto border-t border-border p-3">
          <p className="text-[11px] text-fg-subtle">v0.3.0</p>
        </div>
      </aside>

      {/* Mobile drawer */}
      <div
        className={cn(
          'fixed inset-0 z-40 bg-black/50 lg:hidden',
          open ? 'block' : 'pointer-events-none hidden',
        )}
        onClick={onClose}
        aria-hidden="true"
      />
      <aside
        className={cn(
          'fixed inset-y-0 left-0 z-50 flex w-72 flex-col border-r border-border bg-surface transition-transform duration-200 lg:hidden',
          open ? 'translate-x-0' : '-translate-x-full',
        )}
        aria-hidden={!open}
      >
        <div className="flex h-14 items-center justify-between border-b border-border px-4">
          <div className="flex items-center gap-2">
            <Activity className="size-5 text-accent" aria-hidden="true" />
            <span className="text-sm font-bold tracking-tight text-fg">PudimNetMon</span>
          </div>
          <button
            onClick={onClose}
            aria-label="Close navigation"
            className="rounded-md p-1.5 text-fg-muted hover:bg-surface-muted hover:text-fg"
          >
            <X className="size-4" aria-hidden="true" />
          </button>
        </div>
        {nav}
      </aside>
    </>
  )
}
