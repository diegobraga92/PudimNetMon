import type { InputHTMLAttributes } from 'react'
import { cn } from '../../lib/cn'

interface InputProps extends InputHTMLAttributes<HTMLInputElement> {
  label?: string
  hint?: string
  error?: string
}

export function Input({ label, hint, error, className, id, ...props }: InputProps) {
  const inputId = id ?? (label ? label.toLowerCase().replace(/[^a-z0-9]+/g, '-') : undefined)
  return (
    <div className="flex flex-col gap-1.5">
      {label && (
        <label htmlFor={inputId} className="text-xs font-medium text-fg-muted">
          {label}
        </label>
      )}
      <input
        id={inputId}
        className={cn(
          'h-9 rounded-lg border border-border bg-bg px-3 text-sm text-fg',
          'placeholder:text-fg-subtle',
          'focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/30',
          error && 'border-critical focus:border-critical focus:ring-critical/30',
          className,
        )}
        {...props}
      />
      {error ? (
        <p className="text-xs text-critical" role="alert">
          {error}
        </p>
      ) : hint ? (
        <p className="text-xs text-fg-subtle">{hint}</p>
      ) : null}
    </div>
  )
}
