import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

interface EmptyStateProps {
  icon?: ReactNode
  title: string
  description?: string
  action?: ReactNode
  className?: string
}

export function EmptyState({ icon, title, description, action, className }: EmptyStateProps) {
  return (
    <div
      className={cn(
        'flex flex-col items-center justify-center gap-2 rounded-xl border-2 border-dashed border-border px-6 py-10 text-center',
        className,
      )}
    >
      {icon && <div className="mb-1 text-fg-subtle">{icon}</div>}
      <h3 className="text-sm font-semibold text-fg">{title}</h3>
      {description && <p className="max-w-sm text-xs text-fg-muted">{description}</p>}
      {action && <div className="mt-2">{action}</div>}
    </div>
  )
}
