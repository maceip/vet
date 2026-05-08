import "./LeftRail.css";

export default function LeftRail() {
  return (
    <div className="leftRailInner" aria-label="Substrate flow">
      <svg
        className="leftRailConnectors"
        viewBox="0 0 180 760"
        preserveAspectRatio="none"
        aria-hidden="true"
      >
        <defs>
          <marker
            id="leftRailArrow"
            viewBox="0 0 12 12"
            refX="10"
            refY="6"
            markerWidth="7"
            markerHeight="7"
            orient="auto"
          >
            <path d="M 1 1 L 11 6 L 1 11 Z" />
          </marker>
        </defs>

        <path className="leftRailConnectors__spine" d="M 154 96 V 666" />
        <path className="leftRailConnectors__lead" d="M 92 132 H 154" />
        <path className="leftRailConnectors__lead" d="M 92 380 H 154" />
        <path className="leftRailConnectors__lead" d="M 92 626 H 154" />

        <path
          className="leftRailConnectors__step"
          d="M 154 222 V 308"
          markerEnd="url(#leftRailArrow)"
        />
        <path
          className="leftRailConnectors__step"
          d="M 154 470 V 556"
          markerEnd="url(#leftRailArrow)"
        />

        <circle className="leftRailConnectors__node" cx="154" cy="132" r="6" />
        <circle className="leftRailConnectors__node" cx="154" cy="380" r="6" />
        <circle className="leftRailConnectors__node" cx="154" cy="626" r="6" />
      </svg>

      <img
        src="/LiteRT-DPM/art/paage.png"
        alt="Substrate flow: log, memory, certificate"
        className="leftRailArt"
        loading="eager"
        decoding="async"
      />
      <ul className="leftRailLabels" aria-hidden="true">
        <li>LOG</li>
        <li>MEMORY</li>
        <li>CERTIFICATE</li>
      </ul>
    </div>
  );
}
