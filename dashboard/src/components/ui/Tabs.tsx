import * as TabsPrimitive from '@radix-ui/react-tabs'
import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export function Tabs({
  value,
  onValueChange,
  items,
  className,
  listClassName,
}: {
  value: string
  onValueChange: (value: string) => void
  items: { value: string; label: ReactNode; content: ReactNode }[]
  className?: string
  listClassName?: string
}) {
  return (
    <TabsPrimitive.Root value={value} onValueChange={onValueChange} className={cn('w-full', className)}>
      <TabsPrimitive.List
        className={cn(
          'inline-flex h-9 items-center gap-1 rounded-lg bg-surface-muted p-1',
          listClassName,
        )}
      >
        {items.map((item) => (
          <TabsPrimitive.Trigger
            key={item.value}
            value={item.value}
            className={cn(
              'inline-flex h-7 items-center gap-1.5 rounded-md px-3 text-sm font-medium text-fg-muted transition-colors',
              'hover:text-fg focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
              'data-[state=active]:bg-surface data-[state=active]:text-fg data-[state=active]:shadow-sm',
            )}
          >
            {item.label}
          </TabsPrimitive.Trigger>
        ))}
      </TabsPrimitive.List>
      {items.map((item) => (
        <TabsPrimitive.Content key={item.value} value={item.value} className="mt-4 focus:outline-none">
          {item.content}
        </TabsPrimitive.Content>
      ))}
    </TabsPrimitive.Root>
  )
}
