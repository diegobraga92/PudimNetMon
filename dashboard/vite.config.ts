import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  build: {
    rollupOptions: {
      output: {
        // Separate vendor chunks so react and recharts cache independently
        // and the main bundle stays small. The function form avoids the
        // empty-chunk issue when recharts re-exports react.
        manualChunks(id: string) {
          if (!id.includes('node_modules')) return undefined
          // Recharts and its visualization dependencies
          if (
            id.includes('recharts') ||
            id.includes('d3-') ||
            id.includes('victory-vendor') ||
            id.includes('react-smooth') ||
            id.includes('decimal.js-light') ||
            id.includes('lodash.debounce')
          ) {
            return 'recharts'
          }
          // React runtime
          if (
            id.includes('node_modules/react/') ||
            id.includes('node_modules/react-dom/') ||
            id.includes('node_modules/scheduler/')
          ) {
            return 'react'
          }
          return undefined
        },
      },
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test/setup.ts'],
    css: false,
  },
  server: {
    host: '0.0.0.0',
    port: 3000,
    proxy: {
      // Proxy /api/* requests to the collector.
      '/api': {
        target: 'http://collector:8080',
        changeOrigin: true,
      },
    },
  },
})
