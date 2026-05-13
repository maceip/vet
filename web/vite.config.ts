import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// GitHub Pages serves at /vet/ for the project site.
// If the repo is renamed later, update `base` accordingly.
export default defineConfig({
  plugins: [react()],
  base: "/vet/",
  build: {
    outDir: "../docs",
    emptyOutDir: true,
    sourcemap: false,
  },
});
