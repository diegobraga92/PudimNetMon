/** Thin typed wrappers around fetch for the collector REST API. */

export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
  ) {
    super(message)
    this.name = 'ApiError'
  }
}

export async function apiGet<T>(path: string, params?: Record<string, string | number | undefined>): Promise<T> {
  const url = new URL(path, window.location.origin)
  if (params) {
    for (const [key, value] of Object.entries(params)) {
      if (value !== undefined && value !== '' && value !== 'all') {
        url.searchParams.set(key, String(value))
      }
    }
  }
  const resp = await fetch(url.pathname + url.search)
  if (!resp.ok) throw new ApiError(`HTTP ${resp.status}`, resp.status)
  return (await resp.json()) as T
}

export async function apiPost<T>(path: string, body: unknown): Promise<T> {
  const resp = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (!resp.ok) throw new ApiError(await errorMessage(resp), resp.status)
  return (await resp.json()) as T
}

/** Prefers the server-provided `error` field for a friendly toast message. */
async function errorMessage(resp: Response): Promise<string> {
  try {
    const data: unknown = await resp.json()
    if (data && typeof data === 'object' && 'error' in data) {
      const msg = (data as { error?: unknown }).error
      if (typeof msg === 'string' && msg.length > 0) return msg
    }
  } catch {
    /* not JSON — fall through */
  }
  return `HTTP ${resp.status}`
}

export async function apiPostForm<T>(path: string, body: Record<string, string>): Promise<T> {
  const resp = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(body),
  })
  if (!resp.ok) throw new ApiError(`HTTP ${resp.status}`, resp.status)
  return (await resp.json()) as T
}
