# web/ — replayable-agent-memory hero site

Vite + React + TypeScript + react-three-fiber. Builds to repo-root `docs/`,
deployed by `.github/workflows/pages.yml` on push to `main`.

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

Output: repo-root `docs/`. The Actions workflow uploads that as the Pages artifact.

## Notes

- `vite.config.ts` `base: "/vet/"` — pinned to the repo's Pages path. If
  the repo is renamed, update both `base` here and the font URLs in
  `src/styles/global.css`.
- Fonts and chart PNGs are static assets under `public/`.
- The 3D spin block uses `@react-three/fiber` + `@react-three/drei`. CSS is
  scoped per-component (`*.css` next to the `*.tsx`).
