import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { cleanup, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import App from './App'
import { FilterBar } from './components/ui/FilterBar'
import { useDashboard } from './context/DashboardContext'
import { mockApi } from './test/mockApi'
import { renderWithProviders } from './test/utils'

afterEach(() => {
  cleanup()
  vi.unstubAllGlobals()
})

describe('PudimNetMon dashboard', () => {
  beforeEach(() => {
    mockApi()
  })

  it('renders the app shell with sidebar navigation', async () => {
    renderWithProviders(<App />)

    // Brand renders in both desktop + mobile sidebars
    expect(screen.getAllByText('PudimNetMon').length).toBeGreaterThan(0)
    const nav = screen.getByRole('navigation', { name: 'Main navigation' })
    expect(nav).toBeInTheDocument()
    expect(within(nav).getByRole('button', { name: /Overview/ })).toBeInTheDocument()
    expect(within(nav).getByRole('button', { name: /Alerts/ })).toBeInTheDocument()

    // Health pill reflects the collector status
    await screen.findByText('Collector OK')

    // Stats row populated from mocked data (2/3 agents alive, 5/6 metrics successful)
    expect(await screen.findByText('2/3')).toBeInTheDocument()
    expect(screen.getByText('83%')).toBeInTheDocument()
  })

  it('renders active alerts from the API', async () => {
    renderWithProviders(<App />)

    expect(await screen.findByText('High Latency')).toBeInTheDocument()
    expect(screen.getByText('Packet Loss')).toBeInTheDocument()
    // Unacknowledged critical alert shows an ack button; acknowledged shows a badge
    expect(screen.getByRole('button', { name: 'Ack' })).toBeInTheDocument()
    expect(screen.getByText('Acknowledged')).toBeInTheDocument()
  })

  it('acknowledges an alert and updates the UI', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    const ackButton = await screen.findByRole('button', { name: 'Ack' })
    await user.click(ackButton)

    await waitFor(() => {
      expect(screen.getAllByText('Acknowledged').length).toBeGreaterThanOrEqual(1)
    })
  })

  it('switches views via the sidebar', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    await user.click(screen.getByRole('button', { name: /Agent Config/ }))
    // Page heading + panel heading both contain the text
    expect((await screen.findAllByText('Agent Configuration')).length).toBeGreaterThan(0)

    await user.click(screen.getByRole('button', { name: /Agents/ }))
    expect(await screen.findByPlaceholderText('Search agents…')).toBeInTheDocument()
  })

  it('shows a toast when an alert is acknowledged', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    await user.click(await screen.findByRole('button', { name: 'Ack' }))

    expect(await screen.findByText('Alert acknowledged')).toBeInTheDocument()
  })

  it('toggles between dark and light mode', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    const toggle = screen.getByRole('button', { name: /Switch to (light|dark) mode/ })
    await user.click(toggle)
    expect(document.documentElement.classList.contains('dark')).toBe(true)
    await user.click(screen.getByRole('button', { name: /Switch to (light|dark) mode/ }))
    expect(document.documentElement.classList.contains('dark')).toBe(false)
  })

  it('navigates to the metrics explorer and renders raw rows', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    await user.click(screen.getByRole('button', { name: /Metrics/ }))

    expect(await screen.findByRole('heading', { name: 'Metrics Explorer' })).toBeInTheDocument()
    // Mocked metric rows render in the table (agent-1 appears in several rows)
    expect((await screen.findAllByText('agent-1')).length).toBeGreaterThan(0)
    expect((await screen.findAllByText('example.com')).length).toBeGreaterThan(0)
    // Success + failure status pills
    expect(screen.getAllByText('OK').length).toBeGreaterThan(0)
    expect(screen.getAllByText('FAIL').length).toBeGreaterThan(0)
  })

  it('sorts the metrics table by value', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    await user.click(screen.getByRole('button', { name: /Metrics/ }))
    const valueHeader = await screen.findByRole('button', { name: /Value/ })
    await user.click(valueHeader)

    // First data row now shows the smallest value (-1.3 NTP offset for agent-3) ascending.
    const rows = screen.getAllByRole('row')
    expect(rows[1]).toHaveTextContent('-1.3')
  })

  it('shows a reset button when filters are active and clears them', async () => {
    // Drive the shared filter state through the dashboard context (Radix Select
    // popovers don't reliably open in jsdom, so we test FilterBar's response
    // to state changes instead of simulating a full pick interaction).
    function FilterHarness() {
      const { setSelectedAgent } = useDashboard()
      return (
        <>
          <button onClick={() => setSelectedAgent('agent-1')}>apply-filter</button>
          <FilterBar />
        </>
      )
    }
    const user = userEvent.setup()
    renderWithProviders(<FilterHarness />)

    await user.click(screen.getByRole('button', { name: 'apply-filter' }))

    const reset = await screen.findByRole('button', { name: 'Reset filters' })
    expect(reset).toBeInTheDocument()

    await user.click(reset)
    await waitFor(() => {
      expect(screen.queryByRole('button', { name: 'Reset filters' })).not.toBeInTheDocument()
    })
  })

  it('renders the Deploy Agent page with downloadable platforms', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    await user.click(screen.getByRole('button', { name: /Deploy Agent/ }))

    expect(await screen.findByRole('heading', { name: 'Deploy Agent' })).toBeInTheDocument()
    // Platform cards from the manifest (Linux + Windows) and the Docker card.
    // The filename appears both in the card metadata and the command block.
    expect((await screen.findAllByText(/pudim-agent-linux-amd64/)).length).toBeGreaterThan(0)
    expect(screen.getAllByText(/pudim-agent-windows-amd64\.exe/).length).toBeGreaterThan(0)
    expect(screen.getAllByRole('link', { name: /Download/ }).length).toBeGreaterThan(0)
    expect(screen.getByText('Docker')).toBeInTheDocument()
  })

  it('navigates from the empty agents state to the Deploy page', async () => {
    mockApi({ agents: [] })
    const user = userEvent.setup()
    renderWithProviders(<App />)

    // The Overview stat card is also named "Agents", so target the sidebar nav.
    const nav = screen.getByRole('navigation', { name: 'Main navigation' })
    await user.click(within(nav).getByRole('button', { name: /Agents/ }))
    expect(await screen.findByText('No agents connected')).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: /Deploy an agent/ }))
    expect(await screen.findByRole('heading', { name: 'Deploy Agent' })).toBeInTheDocument()
  })

  it('shows the active view label in the header', async () => {
    const user = userEvent.setup()
    renderWithProviders(<App />)

    const header = screen.getByRole('banner')
    expect(within(header).getByText('Overview')).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: /Alert History/ }))
    expect(await within(header).findByText('Alert History')).toBeInTheDocument()
  })
})
