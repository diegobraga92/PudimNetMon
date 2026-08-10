import { cn } from '../../lib/cn'

/** Pulsing placeholder while data loads. */

export function ChartSkeleton({ height = 320, className }: { height?: number; className?: string }) {
  return (
    <div
      aria-hidden="true"
      className={cn('animate-pulse rounded-xl bg-surface-muted', className)}
      style={{ height }}
    />
  )
}

export function CardSkeleton({ className }: { className?: string }) {
  return (
    <div aria-hidden="true" className={cn('space-y-3', className)}>
      <div className="h-3 w-24 animate-pulse rounded bg-surface-muted" />
      <div className="h-8 w-16 animate-pulse rounded bg-surface-muted" />
    </div>
  )
}

export function ListSkeleton({ rows = 5, className }: { rows?: number; className?: string }) {
  return (
    <div aria-hidden="true" className={cn('space-y-2.5', className)}>
      {Array.from({ length: rows }).map((_, i) => (
        <div key={i} className="flex items-center gap-3">
          <div className="size-2.5 animate-pulse rounded-full bg-surface-muted" />
          <div className="h-4 flex-1 animate-pulse rounded bg-surface-muted" />
        </div>
      ))}
    </div>
  )
}
