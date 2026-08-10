/* PudimNetMon service worker — app-shell caching for offline/PWA support.
 *
 * Strategy:
 *  - Navigation requests: network-first, fall back to the cached index.html.
 *  - /assets/* (content-hashed, immutable): cache-first.
 *  - Everything else (/api/*, /manifest.webmanifest, etc.): network only.
 */
const CACHE_NAME = 'pudim-shell-v1'
const APP_SHELL = ['/', '/index.html', '/manifest.webmanifest', '/favicon.svg', '/icon-192.png', '/icon-512.png']

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches
      .open(CACHE_NAME)
      .then((cache) => cache.addAll(APP_SHELL))
      .then(() => self.skipWaiting()),
  )
})

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  )
})

self.addEventListener('fetch', (event) => {
  const { request } = event
  if (request.method !== 'GET') return

  const url = new URL(request.url)
  if (url.origin !== self.location.origin) return

  // Never cache API responses.
  if (url.pathname.startsWith('/api/')) return

  if (url.pathname.startsWith('/assets/')) {
    // Hashed, immutable assets — cache-first.
    event.respondWith(
      caches.match(request).then((cached) => cached || fetch(request).then((resp) => {
        const clone = resp.clone()
        caches.open(CACHE_NAME).then((cache) => cache.put(request, clone))
        return resp
      })),
    )
    return
  }

  // Navigation / app shell — network-first with offline fallback.
  if (request.mode === 'navigate' || url.pathname === '/') {
    event.respondWith(
      fetch(request)
        .then((resp) => {
          const clone = resp.clone()
          caches.open(CACHE_NAME).then((cache) => cache.put('/index.html', clone))
          return resp
        })
        .catch(() => caches.match('/index.html')),
    )
  }
})
