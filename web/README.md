# web/ — replayable-agent-memory hero site

Vite + React + TypeScript + react-three-fiber. Builds to `web/dist/`, deployed
by `.github/workflows/pages.yml` on push to `phase2-bench`.

## Local dev

```
cd web
npm install
npm run dev
```

## Build

```
npm run build
```

Output: `web/dist/`. The Actions workflow uploads that as the Pages artifact.

## Notes

- `vite.config.ts` `base: "/LiteRT-DPM/"` — pinned to the repo's Pages path. If
  the repo is renamed, update both `base` here and the font URLs in
  `src/styles/global.css`.
- Fonts and chart PNGs are static assets under `public/`.
- The 3D spin block uses `@react-three/fiber` + `@react-three/drei`. CSS is
  scoped per-component (`*.css` next to the `*.tsx`).
