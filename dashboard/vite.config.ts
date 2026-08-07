import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  build: {
    rollupOptions: {
      output: {
        // Split vendor libs so recharts (the heavy dependency) and react are
        // cached independently and the main bundle stays small.
        manualChunks: {
          react: ['react', 'react-dom'],
          recharts: ['recharts'],
        },
      },
    },
  },
  server: {
    host: '0.0.0.0',
    port: 3000,
    proxy: {
      // Pass /api/* through unchanged — the collector serves the dashboard
      // REST API under the /api namespace (see collector/src/main.cpp).
      '/api': {
        target: 'http://collector:8080',
        changeOrigin: true,
      },
    },
  },
})