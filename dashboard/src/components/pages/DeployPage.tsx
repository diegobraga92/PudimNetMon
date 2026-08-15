import { DownloadCloud } from 'lucide-react'
import { useAgentVersions } from '../../hooks/useAgentVersions'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { DockerCard } from '../deploy/DockerCard'
import { PlatformCard, collectorHost } from '../deploy/PlatformCard'

export function DeployPage() {
  const { data, isLoading, isError } = useAgentVersions()
  const platforms = data?.platforms ?? []
  const host = collectorHost()

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Deploy Agent</h1>
        <p className="text-base text-fg-muted">
          Download the pudim-agent daemon and point it at this collector
          (<span className="font-mono">{host}:50051</span>). The agent runs on
          Linux and Windows hosts and reports into the same stack over gRPC.
        </p>
      </div>

      {isLoading ? (
        <Card className="p-5">
          <ListSkeleton rows={3} />
        </Card>
      ) : isError ? (
        <EmptyState
          icon={<DownloadCloud className="size-8" aria-hidden="true" />}
          title="Download manifest unavailable"
          description="The collector did not respond. Check that the dashboard can reach the collector API, then refresh."
        />
      ) : (
        <div className="grid gap-4 md:grid-cols-2">
          {platforms.length === 0 && (
            <div className="md:col-span-2">
              <EmptyState
                icon={<DownloadCloud className="size-8" aria-hidden="true" />}
                title="No prebuilt agent binaries on this collector"
                description="The collector has no staged binaries to serve. Use the Docker image below, or build the agent from source (see docs/deployment.md and docs/windows.md)."
              />
            </div>
          )}
          {platforms.map((platform) => (
            <PlatformCard key={platform.id} platform={platform} />
          ))}
          <DockerCard />
        </div>
      )}
    </div>
  )
}
