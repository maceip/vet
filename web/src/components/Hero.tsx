import { useState } from "react";
import Decor from "./Decor";
import LeftRail from "./LeftRail";
import ReplayableSpinBlock from "./ReplayableSpinBlock";
import Story from "./Story";

export function Hero() {
  const [activePanelIndex, setActivePanelIndex] = useState(0);

  return (
    <main className="heroPage">
      <Decor />

      <section className="heroGrid">
        <aside className="leftRail">
          <LeftRail />
          <RailToCubeWires />
        </aside>

        <div className="heroMainColumn">
          <ReplayableSpinBlock
            activeIndex={activePanelIndex}
            onActiveIndexChange={setActivePanelIndex}
          />
          <Story activeIndex={activePanelIndex} />
        </div>
      </section>

      <SiteFooter />
    </main>
  );
}

function SiteFooter() {
  return (
    <footer className="siteFooter" aria-label="Replayable Systems footer">
      <div className="siteFooter__brand" aria-label="Replayable Systems">
        <span className="siteFooter__mark">R</span>
        <span>
          <strong>REPLAYABLE</strong>
          <em>SYSTEMS</em>
        </span>
      </div>
      <p>Building auditable AI systems on a trustless substrate.</p>
      <span className="siteFooter__copyright">© 2026</span>
    </footer>
  );
}

/**
 * Decorative wires connecting the LeftRail (LOG / MEMORY / CERTIFICATE) to the
 * cube. Lives inside the leftRail aside, absolutely positioned, painting on top
 * of the rail at the right edge.
 */
function RailToCubeWires() {
  return (
    <svg
      className="railWires"
      viewBox="0 0 120 600"
      preserveAspectRatio="none"
      aria-hidden="true"
    >
      <defs>
        <linearGradient id="wireFade" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0%" stopColor="#dce452" stopOpacity="0.18" />
          <stop offset="55%" stopColor="#dce452" stopOpacity="0.72" />
          <stop offset="100%" stopColor="#dce452" stopOpacity="0.92" />
        </linearGradient>
        <marker
          id="cubeWireArrow"
          viewBox="0 0 12 12"
          refX="10"
          refY="6"
          markerWidth="7"
          markerHeight="7"
          orient="auto"
        >
          <path d="M 1 1 L 11 6 L 1 11 Z" fill="#dce452" />
        </marker>
      </defs>

      {/* From LOG node */}
      <path
        d="M 0 112 H 78 Q 104 112 104 138 V 166 H 120"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.8"
        markerEnd="url(#cubeWireArrow)"
      />
      <circle cx="0" cy="112" r="3.5" fill="#050604" stroke="#dce452" strokeWidth="1.4" />

      {/* From MEMORY node */}
      <path
        d="M 0 304 H 58 Q 88 304 88 278 V 250 H 120"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.8"
        markerEnd="url(#cubeWireArrow)"
      />
      <circle cx="0" cy="304" r="3.5" fill="#050604" stroke="#dce452" strokeWidth="1.4" />

      {/* From CERTIFICATE node */}
      <path
        d="M 0 520 H 68 Q 104 520 104 484 V 430 H 120"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.8"
        markerEnd="url(#cubeWireArrow)"
      />
      <circle cx="0" cy="520" r="3.5" fill="#050604" stroke="#dce452" strokeWidth="1.4" />
    </svg>
  );
}
