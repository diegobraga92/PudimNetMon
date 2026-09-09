import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import type { CheckTypeFilter } from '../types'
import {
  CHECK_TYPE_OPTIONS,
  DEFAULT_CHECK_FILTER,
  DEFAULT_WINDOW_SECONDS,
  TIME_WINDOWS,
} from '../lib/constants'

export type View = 'overview' | 'metrics' | 'agents' | 'alerts' | 'history' | 'config' | 'deploy'

interface DashboardContextValue {
  view: View
  setView: (view: View) => void
  sidebarOpen: boolean
  setSidebarOpen: (open: boolean) => void
  selectedAgent: string
  setSelectedAgent: (agentId: string) => void
  selectedCheck: CheckTypeFilter
  setSelectedCheck: (check: CheckTypeFilter) => void
  windowSeconds: number
  setWindowSeconds: (seconds: number) => void
}

const DashboardContext = createContext<DashboardContextValue | null>(null)

function isCheckFilter(value: string | null): value is CheckTypeFilter {
  return value != null && CHECK_TYPE_OPTIONS.some((o) => o.value === value)
}

function isTimeWindow(value: number): boolean {
  return TIME_WINDOWS.some((w) => w.value === value)
}

/** Read the filter set out of the query string (`?agent=..&check=..&window=..`). */
function readUrlFilters(): Pick<DashboardContextValue, 'selectedAgent' | 'selectedCheck' | 'windowSeconds'> {
  if (typeof window === 'undefined') {
    return { selectedAgent: 'all', selectedCheck: DEFAULT_CHECK_FILTER, windowSeconds: DEFAULT_WINDOW_SECONDS }
  }
  const params = new URLSearchParams(window.location.search)
  const rawAgent = params.get('agent')
  const rawCheck = params.get('check')
  const rawWindow = Number(params.get('window'))
  return {
    selectedAgent: rawAgent && rawAgent !== 'all' ? rawAgent : 'all',
    selectedCheck: isCheckFilter(rawCheck) ? rawCheck : DEFAULT_CHECK_FILTER,
    windowSeconds: Number.isInteger(rawWindow) && isTimeWindow(rawWindow) ? rawWindow : DEFAULT_WINDOW_SECONDS,
  }
}

export function DashboardProvider({ children }: { children: ReactNode }) {
  const [view, setView] = useState<View>('overview')
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const initial = readUrlFilters()
  const [selectedAgent, setSelectedAgentState] = useState(initial.selectedAgent)
  const [selectedCheck, setSelectedCheck] = useState(initial.selectedCheck)
  const [windowSeconds, setWindowSeconds] = useState(initial.windowSeconds)

  /** Keep the current filters in the URL so views survive refresh and are shareable. */
  useEffect(() => {
    const params = new URLSearchParams(window.location.search)
    if (selectedAgent === 'all') params.delete('agent')
    else params.set('agent', selectedAgent)
    if (selectedCheck === 'all') params.delete('check')
    else params.set('check', selectedCheck)
    if (windowSeconds === DEFAULT_WINDOW_SECONDS) params.delete('window')
    else params.set('window', String(windowSeconds))
    const qs = params.toString()
    window.history.replaceState(null, '', qs ? `?${qs}` : window.location.pathname)
  }, [selectedAgent, selectedCheck, windowSeconds])

  // Support browser back/forward between saved filter states.
  useEffect(() => {
    const onPopState = () => {
      const filters = readUrlFilters()
      setSelectedAgentState(filters.selectedAgent)
      setSelectedCheck(filters.selectedCheck)
      setWindowSeconds(filters.windowSeconds)
    }
    window.addEventListener('popstate', onPopState)
    return () => window.removeEventListener('popstate', onPopState)
  }, [])

  /** Selecting an agent from a card jumps to the overview with that agent applied. */
  const setSelectedAgent = useCallback(
    (agentId: string) => {
      setSelectedAgentState(agentId)
      setView('overview')
      setSidebarOpen(false)
    },
    [],
  )

  const value = useMemo(
    () => ({
      view,
      setView,
      sidebarOpen,
      setSidebarOpen,
      selectedAgent,
      setSelectedAgent,
      selectedCheck,
      setSelectedCheck,
      windowSeconds,
      setWindowSeconds,
    }),
    [view, sidebarOpen, selectedAgent, setSelectedAgent, selectedCheck, windowSeconds],
  )

  return <DashboardContext.Provider value={value}>{children}</DashboardContext.Provider>
}

export function useDashboard(): DashboardContextValue {
  const ctx = useContext(DashboardContext)
  if (!ctx) throw new Error('useDashboard must be used within DashboardProvider')
  return ctx
}
