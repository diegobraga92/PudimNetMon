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
}
