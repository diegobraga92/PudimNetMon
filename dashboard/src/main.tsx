import React from 'react'
import ReactDOM from 'react-dom/client'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import App from './App'
import './index.css'
import { ToastProvider } from './components/ui/toast'
import { TooltipProvider } from './components/ui/Tooltip'

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 3000,
      retry: 1,
      refetchOnWindowFocus: true,
    },
  },
})

// Apply the saved theme before first paint to avoid a flash of the wrong theme.
let storedTheme: string | null = null
try {
  storedTheme = window.localStorage?.getItem('pudim-theme') ?? null
} catch {
  // Storage can be unavailable (blocked cookies / private browsing).
  storedTheme = null
}
const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches
document.documentElement.classList.toggle('dark', storedTheme === 'dark' || (storedTheme == null && prefersDark))

// Register the service worker for offline/PWA support (production builds only).
if (import.meta.env.PROD && 'serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/sw.js').catch(() => {
      // SW registration is best-effort — never block the app on it.
    })
  })
}

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <QueryClientProvider client={queryClient}>
      <TooltipProvider>
        <ToastProvider>
          <App />
        </ToastProvider>
      </TooltipProvider>
    </QueryClientProvider>
  </React.StrictMode>,
)

