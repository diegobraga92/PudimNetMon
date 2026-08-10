import { Component, type ErrorInfo, type ReactNode } from 'react'
import { AlertTriangle, RefreshCw } from 'lucide-react'
import { Button } from './Button'

interface ErrorBoundaryState {
  hasError: boolean
  message: string
}

interface ErrorBoundaryProps {
  children: ReactNode
  fallback?: ReactNode
}

/** Catches render errors in its subtree so one bad widget can't blank the app. */
export class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
  state: ErrorBoundaryState = { hasError: false, message: '' }

  static getDerivedStateFromError(error: unknown): ErrorBoundaryState {
    return {
      hasError: true,
      message: error instanceof Error ? error.message : 'Something went wrong',
    }
  }

  componentDidCatch(error: unknown, info: ErrorInfo) {
    // eslint-disable-next-line no-console
    console.error('[ErrorBoundary]', error, info)
  }

  private reset = () => this.setState({ hasError: false, message: '' })

  render() {
    if (!this.state.hasError) return this.props.children
    if (this.props.fallback) return this.props.fallback
    return (
      <div className="flex flex-col items-center gap-3 rounded-xl border border-critical/40 bg-critical/10 px-6 py-10 text-center">
        <AlertTriangle className="size-8 text-critical" aria-hidden="true" />
        <h3 className="text-sm font-semibold text-fg">This section crashed</h3>
        <p className="max-w-sm text-xs text-fg-muted">{this.state.message}</p>
        <Button variant="outline" size="sm" onClick={this.reset}>
          <RefreshCw className="size-3.5" aria-hidden="true" />
          Reload section
        </Button>
      </div>
    )
  }
}
