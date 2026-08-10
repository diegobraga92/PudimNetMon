import * as SwitchPrimitive from '@radix-ui/react-switch'
import { cn } from '../../lib/cn'

interface SwitchProps {
  checked: boolean
  onCheckedChange: (checked: boolean) => void
  label?: string
  id?: string
  disabled?: boolean
  className?: string
}

export function Switch({ checked, onCheckedChange, label, id, disabled, className }: SwitchProps) {
  const switchId = id ?? (label ? label.toLowerCase().replace(/[^a-z0-9]+/g, '-') : undefined)
  return (
    <span className={cn('inline-flex items-center gap-2', className)}>
      <SwitchPrimitive.Root
        id={switchId}
        checked={checked}
        onCheckedChange={onCheckedChange}
        disabled={disabled}
        className={cn(
          'relative inline-flex h-5 w-9 shrink-0 cursor-pointer items-center rounded-full transition-colors',
          'focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
          'disabled:cursor-not-allowed disabled:opacity-50',
          checked ? 'bg-accent' : 'bg-border-strong',
        )}
      >
        <SwitchPrimitive.Thumb className="block size-4 translate-x-0.5 rounded-full bg-white shadow transition-transform data-[state=checked]:translate-x-[18px]" />
      </SwitchPrimitive.Root>
      {label && (
        <label htmlFor={switchId} className="cursor-pointer text-sm text-fg">
          {label}
        </label>
      )}
    </span>
  )
}
