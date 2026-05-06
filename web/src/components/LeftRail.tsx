import "./LeftRail.css";

export default function LeftRail() {
  return (
    <div className="leftRailInner" aria-label="Substrate flow">
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
