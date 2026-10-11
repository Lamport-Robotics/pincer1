// Builds dist/ as a loadable unpacked MV3 extension: five single-entry Vite bundles
// (background: ESM for the module service worker; content + inpage + takeover: IIFE, since
// classic content scripts cannot import) plus the popup page, manifest and icons.
import { build } from 'vite';
import { cp, mkdir, rename, rm } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });

const base = {
  root,
  configFile: false,
  mode: 'production',
  logLevel: 'warn',
  publicDir: false,
  define: { 'process.env.NODE_ENV': JSON.stringify('production') },
  build: {
    outDir: dist,
    emptyOutDir: false,
    target: 'es2022',
    sourcemap: false,
    minify: false, // readable bundles: anyone can audit what runs on every page
    modulePreload: false,
    chunkSizeWarningLimit: 4000,
  },
};

async function script(name, entry, format) {
  await build({
    ...base,
    build: {
      ...base.build,
      rollupOptions: {
        input: { [name]: join(root, entry) },
        output: {
          format,
          name: format === 'iife' ? `pincer1_${name}` : undefined,
          entryFileNames: '[name].js',
          inlineDynamicImports: true,
          assetFileNames: 'assets/[name]-[hash][extname]',
        },
      },
    },
  });
  console.log(`built ${name}.js (${format})`);
}

await script('background', 'src/background.ts', 'es');
await script('content', 'src/content.ts', 'iife');
await script('inpage', 'src/inpage.ts', 'iife');
await script('takeover', 'src/takeover.ts', 'iife');

await build({
  ...base,
  root: join(root, 'src', 'popup'),
  publicDir: join(root, 'public'),
  build: {
    ...base.build,
    rollupOptions: {
      input: { popup: join(root, 'src', 'popup', 'index.html') },
      output: {
        entryFileNames: 'assets/[name]-[hash].js',
        assetFileNames: 'assets/[name]-[hash][extname]',
      },
    },
  },
});
await rename(join(dist, 'index.html'), join(dist, 'popup.html'));
console.log('built popup.html');

await cp(join(root, 'manifest.json'), join(dist, 'manifest.json'));
console.log('copied manifest.json → dist/');
