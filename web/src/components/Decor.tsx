import "./Decor.css";

/**
 * Art-deco decorative shapes that bracket the hero — gradient blobs, halftone
 * dot fields, geometric corners. Purely visual, no interaction. SVG so they
 * scale cleanly. Positioned absolute via parent .heroPage (relative).
 */
export default function Decor() {
  return (
    <div className="decor" aria-hidden="true">
      {/* Top-left: gradient blob + halftone */}
      <svg className="decor__shape decor__topLeft" viewBox="0 0 320 320">
        <defs>
          <linearGradient id="blobA" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.55" />
            <stop offset="100%" stopColor="#262915" stopOpacity="0" />
          </linearGradient>
          <radialGradient id="dotsA" cx="50%" cy="50%" r="50%">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.7" />
            <stop offset="100%" stopColor="#dce452" stopOpacity="0" />
          </radialGradient>
          <pattern id="dotPatternA" x="0" y="0" width="14" height="14" patternUnits="userSpaceOnUse">
            <circle cx="2" cy="2" r="1.6" fill="#dce452" opacity="0.55" />
          </pattern>
        </defs>
        <path d="M-20 -20 Q160 30 200 180 Q170 110 -20 130 Z" fill="url(#blobA)" />
        <rect x="120" y="20" width="160" height="160" fill="url(#dotPatternA)" mask="url(#fadeMaskA)" />
        <mask id="fadeMaskA">
          <rect x="120" y="20" width="160" height="160" fill="url(#dotsA)" />
        </mask>
      </svg>

      {/* Top-right: blob + halftone */}
      <svg className="decor__shape decor__topRight" viewBox="0 0 360 360">
        <defs>
          <linearGradient id="blobB" x1="1" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.45" />
            <stop offset="100%" stopColor="#262915" stopOpacity="0" />
          </linearGradient>
          <radialGradient id="dotsB" cx="50%" cy="50%" r="50%">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.65" />
            <stop offset="100%" stopColor="#dce452" stopOpacity="0" />
          </radialGradient>
          <pattern id="dotPatternB" x="0" y="0" width="12" height="12" patternUnits="userSpaceOnUse">
            <circle cx="2" cy="2" r="1.4" fill="#dce452" opacity="0.6" />
          </pattern>
          <mask id="fadeMaskB">
            <rect x="40" y="60" width="200" height="200" fill="url(#dotsB)" />
          </mask>
        </defs>
        <path d="M380 -20 Q220 60 200 220 Q260 130 380 150 Z" fill="url(#blobB)" />
        <rect x="40" y="60" width="200" height="200" fill="url(#dotPatternB)" mask="url(#fadeMaskB)" />
      </svg>

      {/* Bottom-right: gradient quarter-circle */}
      <svg className="decor__shape decor__bottomRight" viewBox="0 0 360 360">
        <defs>
          <radialGradient id="blobC" cx="100%" cy="100%" r="100%">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.42" />
            <stop offset="100%" stopColor="#262915" stopOpacity="0" />
          </radialGradient>
          <pattern id="dotPatternC" x="0" y="0" width="14" height="14" patternUnits="userSpaceOnUse">
            <circle cx="2" cy="2" r="1.5" fill="#dce452" opacity="0.5" />
          </pattern>
          <radialGradient id="dotsC" cx="80%" cy="80%" r="60%">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.55" />
            <stop offset="100%" stopColor="#dce452" stopOpacity="0" />
          </radialGradient>
          <mask id="fadeMaskC">
            <rect x="80" y="80" width="220" height="220" fill="url(#dotsC)" />
          </mask>
        </defs>
        <circle cx="380" cy="380" r="280" fill="url(#blobC)" />
        <rect x="80" y="80" width="220" height="220" fill="url(#dotPatternC)" mask="url(#fadeMaskC)" />
      </svg>

      {/* Bottom-left: quarter-circle */}
      <svg className="decor__shape decor__bottomLeft" viewBox="0 0 320 320">
        <defs>
          <radialGradient id="blobD" cx="0%" cy="100%" r="100%">
            <stop offset="0%" stopColor="#dce452" stopOpacity="0.32" />
            <stop offset="100%" stopColor="#262915" stopOpacity="0" />
          </radialGradient>
        </defs>
        <circle cx="-20" cy="340" r="240" fill="url(#blobD)" />
      </svg>
    </div>
  );
}
