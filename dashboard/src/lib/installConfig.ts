/**
 * Config-token helpers for the one-click installer downloads.
 */

export interface InstallConfig {
  /** gRPC collector endpoint, e.g. "collector.lan:50051". */
  collector: string
  /** Optional node id; when omitted the installer derives one (hostname). */
  node?: string
  /** Probe interval in ms. Defaults to the agent default when omitted. */
  interval?: number
}

function encodeBase64Url(input: string): string {
  const bytes = new TextEncoder().encode(input)
  let bin = ''
  for (const b of bytes) bin += String.fromCharCode(b)
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}

function decodeBase64Url(input: string): string {
  let base64 = input.replace(/-/g, '+').replace(/_/g, '/')
  while (base64.length % 4 !== 0) base64 += '='
  const bin = atob(base64)
  const bytes = Uint8Array.from(bin, (c) => c.charCodeAt(0))
  return new TextDecoder().decode(bytes)
}

/** Serializes config into an unpadded base64url token for the download URL. */
export function encodeInstallConfig(config: InstallConfig): string {
  const params = new URLSearchParams()
  params.set('collector', config.collector)
  if (config.node) params.set('node', config.node)
  if (config.interval) params.set('interval', String(config.interval))
  return encodeBase64Url(params.toString())
}

/** Parses a token produced by encodeInstallConfig. Returns null when malformed. */
export function decodeInstallConfig(token: string): InstallConfig | null {
  try {
    const params = new URLSearchParams(decodeBase64Url(token))
    const collector = params.get('collector')
    if (!collector) return null
    const intervalRaw = params.get('interval')
    return {
      collector,
      node: params.get('node') ?? undefined,
      interval: intervalRaw ? Number(intervalRaw) : undefined,
    }
  } catch {
    return null
  }
}

/**
 * Predicts the filename the collector will serve for a download (mirrors the
 * `InstallerDist::DownloadName` decoration: `stem-cfg-<token><ext>`).
 */
export function decoratedInstallerFilename(filename: string, token: string): string {
  for (const ext of ['.tar.gz', '.run', '.exe', '.sh']) {
    if (filename.endsWith(ext)) {
      return `${filename.slice(0, -ext.length)}-cfg-${token}${ext}`
    }
  }
  return `${filename}-cfg-${token}`
}
