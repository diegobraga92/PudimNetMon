import { useTheme } from './useTheme'

/** Theme-aware colors for Recharts (which applies props as SVG attributes). */
export function useChartTheme() {
  const { theme } = useTheme()
  const dark = theme === 'dark'
  return {
    dark,
    grid: dark ? '#21262d' : '#d0d7de',
    axisText: dark ? '#8b949e' : '#57606a',
    tooltipBg: dark ? '#21262d' : '#ffffff',
    tooltipBorder: dark ? '#30363d' : '#d0d7de',
    tooltipText: dark ? '#e6edf3' : '#1f2328',
    tooltipMuted: dark ? '#9198a1' : '#59636e',
    brushFill: dark ? '#1c2129' : '#f6f8fa',
    brushStroke: dark ? '#484f58' : '#8b949e',
    reference: dark ? '#484f58' : '#8b949e',
  }
}
