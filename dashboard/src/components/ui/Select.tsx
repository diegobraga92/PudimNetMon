import * as SelectPrimitive from '@radix-ui/react-select'
import { Check, ChevronDown } from 'lucide-react'
import { cn } from '../../lib/cn'

export interface SelectOption {
  value: string
  label: string
}

interface SelectProps {
  value: string
  onValueChange: (value: string) => void
  options: SelectOption[]
  placeholder?: string
  ariaLabel?: string
  disabled?: boolean
  className?: string
}

export function Select({
  value,
  onValueChange,
  options,
  placeholder = 'Select…',
  ariaLabel,
  disabled,
  className,
}: SelectProps) {
  return (
    <SelectPrimitive.Root value={value} onValueChange={onValueChange} disabled={disabled}>
      <SelectPrimitive.Trigger
        aria-label={ariaLabel}
        className={cn(
          'inline-flex h-9 w-full items-center justify-between gap-2 rounded-lg border border-border bg-surface px-3 text-sm text-fg',
          'focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/30',
          'disabled:cursor-not-allowed disabled:opacity-50',
          'data-[placeholder]:text-fg-subtle',
          className,
        )}
      >
        <SelectPrimitive.Value placeholder={placeholder} />
        <SelectPrimitive.Icon>
          <ChevronDown className="size-4 text-fg-muted" aria-hidden="true" />
        </SelectPrimitive.Icon>
      </SelectPrimitive.Trigger>

      <SelectPrimitive.Portal>
        <SelectPrimitive.Content
          position="popper"
          sideOffset={4}
          className="z-50 max-h-72 min-w-[var(--radix-select-trigger-width)] overflow-y-auto rounded-lg border border-border bg-surface-raised shadow-xl"
        >
          <SelectPrimitive.Viewport className="p-1">
            {options.map((option) => (
              <SelectPrimitive.Item
                key={option.value}
                value={option.value}
                className={cn(
                  'relative flex cursor-pointer select-none items-center rounded-md py-1.5 pl-8 pr-3 text-sm text-fg',
                  'focus:bg-accent focus:text-white focus:outline-none',
                  'data-[state=checked]:font-medium',
                )}
              >
                <span className="absolute left-2 flex size-4 items-center justify-center">
                  <SelectPrimitive.ItemIndicator>
                    <Check className="size-3.5" aria-hidden="true" />
                  </SelectPrimitive.ItemIndicator>
                </span>
                <SelectPrimitive.ItemText>{option.label}</SelectPrimitive.ItemText>
              </SelectPrimitive.Item>
            ))}
          </SelectPrimitive.Viewport>
        </SelectPrimitive.Content>
      </SelectPrimitive.Portal>
    </SelectPrimitive.Root>
  )
}
