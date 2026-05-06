import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// GitHub Pages serves at /LiteRT-DPM/ for the project site.
// If the repo is renamed later, update `base` accordingly.
export default defineConfig({
  plugins: [react()],
  base: "/LiteRT-DPM/",
  build: {
    outDir: "dist",
    sourcemap: false,
  },
});
