import '@testing-library/jest-dom/vitest'

// jsdom lacks these browser APIs that our app and recharts rely on.
if (typeof window !== 'undefined') {
  Object.defineProperty(window, 'matchMedia', {
    writable: true,
    value: (query: string) => ({
      matches: query.includes('prefers-color-scheme: dark') ? false : false,
      media: query,
      onchange: null,
      addListener: () => {},
      removeListener: () => {},
      addEventListener: () => {},
      removeEventListener: () => {},
      dispatchEvent: () => false,
    }),
  })

  globalThis.ResizeObserver = class ResizeObserver {
    observe() {}
    unobserve() {}
    disconnect() {}
  }

  // Recharts measures DOM nodes via getBoundingClientRect.
  if (!Element.prototype.getBoundingClientRect) {
    Element.prototype.getBoundingClientRect = () => ({
      x: 0, y: 0, top: 0, left: 0, bottom: 0, right: 0,
      width: 800, height: 320, toJSON: () => ({}),
    }) as DOMRect
  }

  // localStorage/sessionStorage are not reliably present on the jsdom window
  // across every Node version (they can be undefined on newer Node), so install
  // deterministic in-memory polyfills instead of depending on jsdom internals.
  const createStorage = (): Storage => {
    const store = new Map<string, string>()
    return {
      get length() { return store.size },
      clear: () => store.clear(),
      getItem: (key: string) => (store.has(key) ? store.get(key)! : null),
      key: (index: number) => [...store.keys()][index] ?? null,
      removeItem: (key: string) => { store.delete(key) },
      setItem: (key: string, value: string) => { store.set(key, String(value)) },
    }
  }
  Object.defineProperty(window, 'localStorage', {
    configurable: true,
    writable: true,
    value: createStorage(),
  })
  Object.defineProperty(window, 'sessionStorage', {
    configurable: true,
    writable: true,
    value: createStorage(),
  })
}
