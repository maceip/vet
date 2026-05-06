import Decor from "./Decor";
import LeftRail from "./LeftRail";
import ReplayableSpinBlock from "./ReplayableSpinBlock";

export function Hero() {
  return (
    <main className="heroPage">
      <Decor />

      <header className="heroTitle">
        <h1>REPLAYABLE</h1>
        <h2>AGENT MEMORY</h2>
        <p>A log-based substrate for auditable AI decisions.</p>
      </header>

      <section className="heroGrid">
        <aside className="leftRail">
          <LeftRail />
          <RailToCubeWires />
        </aside>

        <ReplayableSpinBlock />
      </section>
    </main>
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
          <stop offset="0%" stopColor="#dce452" stopOpacity="0.0" />
          <stop offset="40%" stopColor="#dce452" stopOpacity="0.55" />
          <stop offset="100%" stopColor="#dce452" stopOpacity="0.85" />
        </linearGradient>
      </defs>

      {/* From LOG node */}
      <path
        d="M 6 70 Q 60 70 95 130 L 120 130"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.4"
        strokeDasharray="3 4"
      />
      <circle cx="6" cy="70" r="3" fill="#dce452" />
      <circle cx="120" cy="130" r="2.5" fill="#dce452" opacity="0.7" />

      {/* From MEMORY node */}
      <path
        d="M 6 300 Q 60 300 95 300 L 120 300"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.4"
        strokeDasharray="3 4"
      />
      <circle cx="6" cy="300" r="3" fill="#dce452" />
      <circle cx="120" cy="300" r="2.5" fill="#dce452" opacity="0.7" />

      {/* From CERTIFICATE node */}
      <path
        d="M 6 530 Q 60 530 95 470 L 120 470"
        fill="none"
        stroke="url(#wireFade)"
        strokeWidth="1.4"
        strokeDasharray="3 4"
      />
      <circle cx="6" cy="530" r="3" fill="#dce452" />
      <circle cx="120" cy="470" r="2.5" fill="#dce452" opacity="0.7" />
    </svg>
  );
}
