import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { Loader2 } from 'lucide-react'
import { cn } from '../../lib/cn'

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'ghost' | 'danger' | 'outline'
  size?: 'sm' | 'md' | 'lg'
  loading?: boolean
  children?: ReactNode
}

const variantClasses: Record<string, string> = {
  primary:
    'bg-accent text-white hover:bg-accent/90 focus-visible:ring-accent/40 disabled:hover:bg-accent',
  secondary:
    'bg-surface-raised text-fg border border-border hover:bg-surface-muted focus-visible:ring-border',
  outline:
    'border border-border-strong text-fg hover:bg-surface-muted focus-visible:ring-border',
  ghost: 'text-fg-muted hover:text-fg hover:bg-surface-muted focus-visible:ring-border',
  danger:
    'bg-critical/10 text-critical border border-critical/40 hover:bg-critical/20 focus-visible:ring-critical/30',
}

const sizeClasses = {
  sm: 'h-7 px-2.5 text-xs gap-1.5',
  md: 'h-9 px-4 text-sm gap-2',
  lg: 'h-10 px-5 text-sm gap-2',
}

export function Button({
  variant = 'primary',
  size = 'md',
  loading = false,
  className,
  disabled,
  children,
  type = 'button',
  ...props
}: ButtonProps) {
  return (
    <button
      type={type}
      className={cn(
        'inline-flex items-center justify-center rounded-lg font-medium transition-colors',
        'focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:ring-offset-bg',
        'disabled:cursor-not-allowed disabled:opacity-50',
        variantClasses[variant],
        sizeClasses[size],
        className,
      )}
      disabled={disabled || loading}
      {...props}
    >
      {loading && <Loader2 className="size-4 animate-spin" aria-hidden="true" />}
      {children}
    </button>
  )
}
