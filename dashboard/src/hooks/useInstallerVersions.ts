import { useQuery } from '@tanstack/react-query'
import { apiGet } from '../lib/api'
import type { InstallerVersionsResponse } from '../types'

/** Metadata for the single-file installer artifacts the collector serves. */
export function useInstallerVersions() {
  return useQuery({
    queryKey: ['installer-versions'],
    queryFn: () => apiGet<InstallerVersionsResponse>('/api/installers/versions'),
    refetchInterval: 60_000,
    retry: 1,
  })
}
