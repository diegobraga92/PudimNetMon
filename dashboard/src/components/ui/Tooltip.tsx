import * as TooltipPrimitive from '@radix-ui/react-tooltip'
import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export function TooltipProvider({ children }: { children: ReactNode }) {
  return <TooltipPrimitive.Provider delayDuration={200}>{children}</TooltipPrimitive.Provider>
}

interface TooltipProps {
  content: ReactNode
  children: ReactNode
  className?: string
}

export function Tooltip({ content, children, className }: TooltipProps) {
  return (
    <TooltipPrimitive.Root>
      <TooltipPrimitive.Trigger asChild>{children}</TooltipPrimitive.Trigger>
      <TooltipPrimitive.Portal>
        <TooltipPrimitive.Content
          sideOffset={6}
          className={cn(
            'z-50 max-w-xs rounded-lg border border-border bg-surface-raised px-3 py-1.5 text-xs text-fg shadow-lg',
            'data-[state=delayed-open]:animate-in data-[state=closed]:animate-out',
            className,
          )}
        >
          {content}
          <TooltipPrimitive.Arrow className="fill-surface-raised" />
        </TooltipPrimitive.Content>
      </TooltipPrimitive.Portal>
    </TooltipPrimitive.Root>
  )
}
