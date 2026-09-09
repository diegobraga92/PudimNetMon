import { DownloadCloud } from 'lucide-react'
import { useAgentVersions } from '../../hooks/useAgentVersions'
import { useInstallerVersions } from '../../hooks/useInstallerVersions'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { DockerCard } from '../deploy/DockerCard'
import { InstallerCard } from '../deploy/InstallerCard'
import { PlatformCard, collectorHost } from '../deploy/PlatformCard'

export function DeployPage() {
  const { data, isLoading, isError } = useAgentVersions()
  const installers = useInstallerVersions()
  const platforms = data?.platforms ?? []
  const host = collectorHost()
  const hasInstallers = (installers.data?.installers.length ?? 0) > 0
  const showNoBinaries =
    platforms.length === 0 && (installers.data?.installers.length ?? 0) === 0

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Deploy Agent</h1>
        <p className="text-base text-fg-muted">
          Install the pudim-agent on Linux and Windows hosts and point it at this
          collector (<span className="font-mono">{host}:50051</span>). Single-file
          installers carry the collector config in their file name; raw binaries
          are also listed below for scripting.
        </p>
      </div>

      {isLoading || installers.isLoading ? (
        <Card className="p-5">
          <ListSkeleton rows={3} />
        </Card>
      ) : isError || installers.isError ? (
        <EmptyState
          icon={<DownloadCloud className="size-8" aria-hidden="true" />}
          title="Download manifest unavailable"
          description="The collector did not respond. Check that the dashboard can reach the collector API, then refresh."
        />
      ) : (
        <>
          {hasInstallers && (
            <section className="space-y-3" aria-label="Single-file installers">
              <h2 className="text-sm font-semibold text-fg-muted">
                Single-file installers
              </h2>
              <div className="grid gap-4 md:grid-cols-2">
                {installers.data!.installers.map((installer) => (
                  <InstallerCard key={installer.id} installer={installer} />
                ))}
              </div>
            </section>
          )}
          {showNoBinaries && (
            <div className="md:col-span-2">
              <EmptyState
                icon={<DownloadCloud className="size-8" aria-hidden="true" />}
                title="No prebuilt agents on this collector"
                description="The collector has no staged binaries or installers to serve. Use the Docker image below, or build the agent from source."
              />
            </div>
          )}
          <section className="space-y-3" aria-label="Prebuilt agent binaries">
            <h2 className="text-sm font-semibold text-fg-muted">
              Prebuilt agent binaries
            </h2>
            <div className="grid gap-4 md:grid-cols-2">
              {platforms.map((platform) => (
                <PlatformCard key={platform.id} platform={platform} />
              ))}
              <DockerCard />
            </div>
          </section>
        </>
      )}
    </div>
  )
}
