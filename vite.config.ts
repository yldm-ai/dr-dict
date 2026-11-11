import { defineConfig } from 'vite';
export default defineConfig({
  root: 'web',
  base: './',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    target: 'es2022',
    sourcemap: false,
    // The main window and the Ctrl+C+C popup are separate pages sharing modules and styles.
    rollupOptions: { input: { index: 'web/index.html', popup: 'web/popup.html' } },
  },
});
