import { fileURLToPath } from 'node:url';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5182,
    strictPort: true,
    fs: { strict: true, allow: [fileURLToPath(new URL('.', import.meta.url))] },
  },
  preview: { host: '127.0.0.1', port: 5182, strictPort: true },
  build: { target: 'es2024' },
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    restoreMocks: true,
    clearMocks: true,
  },
});
