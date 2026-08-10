import type { HTMLAttributes, ReactNode } from 'react'
import { cn } from '../../lib/cn'

export type BadgeVariant = 'success' | 'warning' | 'critical' | 'info' | 'neutral'

const variantClasses: Record<BadgeVariant, string> = {
  success: 'bg-success/15 text-success border-success/30',
  warning: 'bg-warning/15 text-warning border-warning/30',
  critical: 'bg-critical/15 text-critical border-critical/30',
  info: 'bg-info/15 text-info border-info/30',
  neutral: 'bg-surface-muted text-fg-muted border-border',
}

interface BadgeProps extends HTMLAttributes<HTMLSpanElement> {
  variant?: BadgeVariant
  children?: ReactNode
}

export function Badge({ variant = 'neutral', className, children, ...props }: BadgeProps) {
  return (
    <span
      className={cn(
        'inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-[11px] font-medium uppercase tracking-wide',
        variantClasses[variant],
        className,
      )}
      {...props}
    >
      {children}
    </span>
  )
}
