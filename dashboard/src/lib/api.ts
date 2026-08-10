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
  if (!resp.ok) throw new ApiError(`HTTP ${resp.status}`, resp.status)
  return (await resp.json()) as T
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
