import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'
import { Card } from '../ui/Card'
import { ChartSkeleton } from '../ui/LoadingSkeleton'
import { EmptyState } from '../ui/EmptyState'

interface ChartContainerProps {
  title: string
  subtitle?: string
  action?: ReactNode
  isLoading: boolean
  isEmpty: boolean
  emptyMessage?: string
  emptyIcon?: ReactNode
  children: ReactNode
  className?: string
}

/** Consistent card wrapper for charts with loading/empty state handling. */
export function ChartContainer({
  title,
  subtitle,
  action,
  isLoading,
  isEmpty,
  emptyMessage,
  emptyIcon,
  children,
  className,
}: ChartContainerProps) {
  return (
    <Card className={cn('p-5', className)}>
      <div className="mb-4 flex flex-wrap items-center justify-between gap-2">
        <div>
          <h2 className="text-sm font-semibold text-fg">{title}</h2>
          {subtitle && <p className="mt-0.5 text-xs text-fg-muted">{subtitle}</p>}
        </div>
        {action}
      </div>
      {isLoading ? (
        <ChartSkeleton height={260} />
      ) : isEmpty ? (
        <EmptyState icon={emptyIcon} title={title} description={emptyMessage} />
      ) : (
        children
      )}
    </Card>
  )
}
