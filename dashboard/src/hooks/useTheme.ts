import { useCallback, useEffect, useState } from 'react'

export type Theme = 'dark' | 'light'

const STORAGE_KEY = 'pudim-theme'

function getInitialTheme(): Theme {
  if (typeof window === 'undefined') return 'dark'
  let stored: string | null = null
  try {
    stored = window.localStorage?.getItem(STORAGE_KEY) ?? null
  } catch {
    // Storage can be unavailable (blocked cookies / private browsing).
    stored = null
  }
  if (stored === 'dark' || stored === 'light') return stored
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

/** Theme state persisted to localStorage and reflected on <html class="dark">. */
export function useTheme() {
  const [theme, setTheme] = useState<Theme>(getInitialTheme)

  useEffect(() => {
    document.documentElement.classList.toggle('dark', theme === 'dark')
    try {
      window.localStorage?.setItem(STORAGE_KEY, theme)
    } catch {
      // Storage unavailable — the theme still applies for this session.
    }
  }, [theme])

  const toggleTheme = useCallback(() => {
    setTheme((t) => (t === 'dark' ? 'light' : 'dark'))
  }, [])

  return { theme, toggleTheme }
}
