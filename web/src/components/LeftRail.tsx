import "./LeftRail.css";

export default function LeftRail() {
  return (
    <div className="leftRailInner" aria-label="Substrate flow">
      <RailNode label="LOG">
        <LogIcon />
      </RailNode>
      <RailArrow />
      <RailNode label="MEMORY">
        <MemoryIcon />
      </RailNode>
      <RailArrow />
      <RailNode label="CERTIFICATE">
        <CertificateIcon />
      </RailNode>
    </div>
  );
}

function RailNode({
  label,
  children,
}: {
  label: string;
  children: React.ReactNode;
}) {
  return (
    <figure className="railNode">
      <figcaption>{label}</figcaption>
      <div className="railGlyph">{children}</div>
    </figure>
  );
}

function RailArrow() {
  return (
    <div className="railArrow" aria-hidden="true">
      <span className="railArrowDot" />
      <svg viewBox="0 0 24 24">
        <path
          d="M12 4v14m-5-5l5 5 5-5"
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
      <span className="railArrowDot" />
    </div>
  );
}

function LogIcon() {
  return (
    <svg viewBox="0 0 80 100" aria-hidden="true">
      <rect x="6" y="6" width="68" height="88" rx="6" fill="none" stroke="currentColor" strokeWidth="1.4" opacity="0.7" />
      {[18, 32, 46, 60, 74].map((y, i) => (
        <g key={y} opacity={1 - i * 0.13}>
          <circle cx="16" cy={y} r="2.4" fill="currentColor" />
          <rect x="24" y={y - 4} width="44" height="8" rx="2" fill="currentColor" opacity="0.42" />
        </g>
      ))}
    </svg>
  );
}

function MemoryIcon() {
  return (
    <svg viewBox="0 0 96 96" aria-hidden="true">
      {[0, 1, 2].map((row) =>
        [0, 1, 2].map((col) => {
          const x = 6 + col * 28;
          const y = 6 + row * 28;
          const isHot = row === 0 && col === 2;
          return (
            <rect
              key={`${row}-${col}`}
              x={x}
              y={y}
              width="22"
              height="22"
              rx="3"
              fill={isHot ? "currentColor" : "rgba(220,228,82,.28)"}
              stroke="currentColor"
              strokeWidth="1.2"
              opacity={isHot ? 1 : 0.85}
            />
          );
        }),
      )}
    </svg>
  );
}

function CertificateIcon() {
  return (
    <svg viewBox="0 0 80 100" aria-hidden="true">
      <path
        d="M40 8l28 14v32c0 16-12 30-28 38C24 84 12 70 12 54V22z"
        fill="rgba(220,228,82,.16)"
        stroke="currentColor"
        strokeWidth="1.6"
      />
      <path
        d="M28 52l9 9 17-19"
        fill="none"
        stroke="currentColor"
        strokeWidth="2.6"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}
