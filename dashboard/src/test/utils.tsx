import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { render } from '@testing-library/react'
import type { ReactElement, ReactNode } from 'react'
import { DashboardProvider } from '../context/DashboardContext'
import { ToastProvider } from '../components/ui/toast'
import { TooltipProvider } from '../components/ui/Tooltip'

/** Renders a component wrapped in the same providers as the real app. */
export function renderWithProviders(ui: ReactElement) {
  const queryClient = new QueryClient({
    defaultOptions: { queries: { retry: false, gcTime: Infinity } },
  })
  const wrapper = ({ children }: { children: ReactNode }) => (
    <QueryClientProvider client={queryClient}>
      <TooltipProvider>
        <ToastProvider>
          <DashboardProvider>{children}</DashboardProvider>
        </ToastProvider>
      </TooltipProvider>
    </QueryClientProvider>
  )
  return {
    queryClient,
    ...render(ui, { wrapper }),
  }
}

