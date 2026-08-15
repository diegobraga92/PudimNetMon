import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from 'react'
import type { CheckTypeFilter } from '../types'

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

export function DashboardProvider({ children }: { children: ReactNode }) {
  const [view, setView] = useState<View>('overview')
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const [selectedAgent, setSelectedAgentState] = useState<string>('all')
  const [selectedCheck, setSelectedCheck] = useState<CheckTypeFilter>('all')
  const [windowSeconds, setWindowSeconds] = useState(300)

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
