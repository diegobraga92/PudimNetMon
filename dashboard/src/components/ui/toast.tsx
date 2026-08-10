import * as ToastPrimitive from '@radix-ui/react-toast'
import { CheckCircle2, Info, XCircle } from 'lucide-react'
import { createContext, useCallback, useContext, useRef, useState, type ReactNode } from 'react'
import { cn } from '../../lib/cn'

type ToastVariant = 'success' | 'error' | 'info'

interface ToastData {
  id: number
  title: string
  description?: string
  variant: ToastVariant
}

interface ToastContextValue {
  toast: (opts: { title: string; description?: string; variant?: ToastVariant }) => void
}

const ToastContext = createContext<ToastContextValue | null>(null)

const VARIANT_STYLES: Record<ToastVariant, { icon: typeof Info; bar: string }> = {
  success: { icon: CheckCircle2, bar: 'bg-success' },
  error: { icon: XCircle, bar: 'bg-critical' },
  info: { icon: Info, bar: 'bg-info' },
}

let nextId = 1

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<ToastData[]>([])
  const timers = useRef<Map<number, number>>(new Map())

  const dismiss = useCallback((id: number) => {
    setToasts((prev) => prev.filter((t) => t.id !== id))
    const timer = timers.current.get(id)
    if (timer) window.clearTimeout(timer)
    timers.current.delete(id)
  }, [])

  const toast = useCallback(
    ({ title, description, variant = 'info' }: { title: string; description?: string; variant?: ToastVariant }) => {
      const id = nextId++
      setToasts((prev) => [...prev.slice(-3), { id, title, description, variant }])
      const timer = window.setTimeout(() => dismiss(id), 4500)
      timers.current.set(id, timer)
    },
    [dismiss],
  )

  return (
    <ToastContext.Provider value={{ toast }}>
      <ToastPrimitive.Provider swipeDirection="right">
        {children}
        <ToastPrimitive.Viewport className="fixed bottom-4 right-4 z-50 flex w-[calc(100vw-2rem)] max-w-sm flex-col gap-2 outline-none" />
        {toasts.map((t) => {
          const style = VARIANT_STYLES[t.variant]
          const Icon = style.icon
          return (
            <ToastPrimitive.Root
              key={t.id}
              onOpenChange={(open) => {
                if (!open) dismiss(t.id)
              }}
              className={cn(
                'pointer-events-auto relative flex w-full items-start gap-3 overflow-hidden rounded-xl border border-border bg-surface-raised p-4 shadow-xl',
                'data-[state=open]:animate-in data-[state=closed]:animate-out data-[swipe=end]:animate-out',
                'data-[state=open]:slide-in-from-bottom-full data-[state=closed]:fade-out-80 data-[swipe=end]:translate-x-full',
              )}
            >
              <span className={cn('absolute inset-y-0 left-0 w-1', style.bar)} aria-hidden="true" />
              <Icon
                className={cn(
                  'mt-0.5 size-4 shrink-0',
                  t.variant === 'success' && 'text-success',
                  t.variant === 'error' && 'text-critical',
                  t.variant === 'info' && 'text-info',
                )}
                aria-hidden="true"
              />
              <div className="min-w-0 flex-1">
                <ToastPrimitive.Title className="text-sm font-semibold text-fg">{t.title}</ToastPrimitive.Title>
                {t.description && (
                  <ToastPrimitive.Description className="mt-0.5 text-xs text-fg-muted">
                    {t.description}
                  </ToastPrimitive.Description>
                )}
              </div>
              <ToastPrimitive.Close
                aria-label="Dismiss notification"
                className="rounded-md p-1 text-fg-subtle transition-colors hover:bg-surface-muted hover:text-fg focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40"
              >
                <span aria-hidden="true" className="text-lg leading-none">×</span>
              </ToastPrimitive.Close>
            </ToastPrimitive.Root>
          )
        })}
      </ToastPrimitive.Provider>
    </ToastContext.Provider>
  )
}

export function useToast(): ToastContextValue {
  const ctx = useContext(ToastContext)
  if (!ctx) throw new Error('useToast must be used within ToastProvider')
  return ctx
}
