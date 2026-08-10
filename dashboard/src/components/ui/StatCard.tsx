import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'
import { Card } from './Card'
import { CardSkeleton } from './LoadingSkeleton'

interface StatCardProps {
  label: string
  value: ReactNode
  hint?: ReactNode
  icon?: ReactNode
  tone?: 'default' | 'success' | 'warning' | 'critical' | 'info'
  loading?: boolean
  onClick?: () => void
}

const toneClasses: Record<NonNullable<StatCardProps['tone']>, string> = {
  default: 'text-fg',
  success: 'text-success',
  warning: 'text-warning',
  critical: 'text-critical',
  info: 'text-info',
}

/** 3px left accent shown for non-default tones. */
const toneAccent: Record<NonNullable<StatCardProps['tone']>, string> = {
  default: '',
  success: 'border-l-success',
  warning: 'border-l-warning',
  critical: 'border-l-critical',
  info: 'border-l-info',
}

export function StatCard({ label, value, hint, icon, tone = 'default', loading, onClick }: StatCardProps) {
  return (
    <Card
      className={cn(
        'flex flex-col gap-1.5 border-l-4 p-4 transition-shadow',
        toneAccent[tone],
        onClick && 'cursor-pointer hover:shadow-md',
      )}
      role={onClick ? 'button' : undefined}
      tabIndex={onClick ? 0 : undefined}
      onClick={onClick}
      onKeyDown={onClick ? (e) => e.key === 'Enter' && onClick() : undefined}
    >
      <div className="flex items-center justify-between">
        <span className="text-xs font-medium uppercase tracking-wide text-fg-muted">{label}</span>
        {icon && <span className="text-fg-subtle">{icon}</span>}
      </div>
      {loading ? (
        <CardSkeleton />
      ) : (
        <>
          <span className={cn('text-2xl font-bold leading-none', toneClasses[tone])}>{value}</span>
          {hint && <span className="text-xs text-fg-subtle">{hint}</span>}
        </>
      )}
    </Card>
  )
}
