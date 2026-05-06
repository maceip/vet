"use client";

import React, { useCallback, useRef, useState } from "react";
import { Canvas, useFrame } from "@react-three/fiber";
import { Edges, Html } from "@react-three/drei";
import * as THREE from "three";
import "./ReplayableSpinBlock.css";

const GRAPH_INDEX = 4;

const FACE_W = 5.6;
const FACE_H = 3.9;
const DEPTH = 5.6;
const HTML_SCALE: [number, number, number] = [0.01, 0.01, 0.01];

type FeatureIcon = "log" | "refresh" | "certificate" | "receipt";

type Feature = {
  number: string;
  title: string;
  body: string;
  icon: FeatureIcon;
};

const FEATURES: Feature[] = [
  {
    number: "1",
    title: "Append-only logs don’t hallucinate",
    body:
      "Every prompt, every tool call, every result is written once and never edited. The log is just what happened. Hallucinations belong to models, not logs.",
    icon: "log",
  },
  {
    number: "2",
    title: "Memory rebuilt at decision time",
    body:
      "The agent reads the log, builds the memory it needs, uses it once, throws it away. Next decision, fresh build.",
    icon: "refresh",
  },
  {
    number: "3",
    title: "Content-addressed audit certificates",
    body:
      "The substrate replays the log to verify every memory the agent uses. If the replay doesn’t match, the agent stops. The certificate is a child node of the checkpoint in a Merkle DAG.",
    icon: "certificate",
  },
  {
    number: "4",
    title: "Decisions with receipts",
    body:
      "Every decision the agent makes points back to the exact events that caused it. Not a paraphrase, not a summary — the events themselves.",
    icon: "receipt",
  },
];

export default function ReplayableSpinBlock() {
  const [featureIndex, setFeatureIndex] = useState(1); // Start on section 2, like the mockup.
  const [showGraphs, setShowGraphs] = useState(false);

  const dragStartX = useRef<number | null>(null);

  const currentIndex = showGraphs ? GRAPH_INDEX : featureIndex;

  const goTo = useCallback((index: number) => {
    if (index === GRAPH_INDEX) {
      setShowGraphs(true);
      return;
    }

    setFeatureIndex(index);
    setShowGraphs(false);
  }, []);

  const next = useCallback(() => {
    const nextIndex = (currentIndex + 1) % 5;
    goTo(nextIndex);
  }, [currentIndex, goTo]);

  const previous = useCallback(() => {
    const previousIndex = (currentIndex + 4) % 5;
    goTo(previousIndex);
  }, [currentIndex, goTo]);

  function onPointerDown(event: React.PointerEvent<HTMLElement>) {
    dragStartX.current = event.clientX;
  }

  function onPointerUp(event: React.PointerEvent<HTMLElement>) {
    if (dragStartX.current === null) return;

    const deltaX = event.clientX - dragStartX.current;
    dragStartX.current = null;

    if (Math.abs(deltaX) < 48) return;
    if (deltaX < 0) next();
    else previous();
  }

  return (
    <section
      className="spinBlockShell"
      aria-label="Replayable agent memory carousel"
      onPointerDown={onPointerDown}
      onPointerUp={onPointerUp}
    >
      <Canvas
        dpr={[1, 1.75]}
        camera={{ position: [0, 0.15, 9.2], fov: 39 }}
        gl={{ antialias: true, alpha: true }}
      >
        <ambientLight intensity={0.72} />
        <directionalLight
          position={[3.5, 4.5, 5]}
          intensity={2.1}
          color="#e7ec57"
        />
        <pointLight position={[-4, -2, 4]} intensity={0.9} color="#dce35a" />

        <SpinBlock featureIndex={featureIndex} showGraphs={showGraphs} />
      </Canvas>

      <button
        className="spinArrow spinArrowPrev"
        type="button"
        aria-label="Previous panel"
        onClick={previous}
      >
        ‹
      </button>

      <button
        className="spinArrow spinArrowNext"
        type="button"
        aria-label="Next panel"
        onClick={next}
      >
        ›
      </button>

      <nav className="spinTabs" aria-label="Carousel panels">
        {["1", "2", "3", "4", "Graphs"].map((label, index) => (
          <button
            key={label}
            type="button"
            className={currentIndex === index ? "isActive" : ""}
            aria-pressed={currentIndex === index}
            onClick={() => goTo(index)}
          >
            {label}
          </button>
        ))}
      </nav>

      <div className="spinHint" aria-hidden="true">
        <span>↔</span>
        <span>Swipe / rotate for graphs</span>
      </div>
    </section>
  );
}

function SpinBlock({
  featureIndex,
  showGraphs,
}: {
  featureIndex: number;
  showGraphs: boolean;
}) {
  const groupRef = useRef<THREE.Group | null>(null);

  useFrame((_, delta) => {
    if (!groupRef.current) return;

    // Each of the 4 features lives on a distinct face of the cube,
    // rotated -π/2 apart on Y. Graphs lives on top (X-axis tilt).
    const targetY = showGraphs ? 0 : -featureIndex * (Math.PI / 2);
    const targetX = showGraphs ? Math.PI / 2 : 0;

    groupRef.current.rotation.y = THREE.MathUtils.damp(
      groupRef.current.rotation.y,
      targetY,
      7,
      delta,
    );
    groupRef.current.rotation.x = THREE.MathUtils.damp(
      groupRef.current.rotation.x,
      targetX,
      7,
      delta,
    );
  });

  return (
    <group ref={groupRef}>
      <mesh>
        <boxGeometry args={[FACE_W, FACE_H, DEPTH]} />
        <meshStandardMaterial
          color="#0b0e08"
          roughness={0.85}
          metalness={0.12}
          transparent
          opacity={0.12}
          depthWrite={false}
        />
        <Edges scale={1.002} color="#d9df4f" threshold={15} linewidth={1} />
      </mesh>

      {/* Front: feature 1 */}
      <HtmlFace position={[0, 0, DEPTH / 2 + 0.01]} rotation={[0, 0, 0]}>
        <FeatureFace feature={FEATURES[0]} />
      </HtmlFace>

      {/* Right: feature 2 */}
      <HtmlFace
        position={[FACE_W / 2 + 0.01, 0, 0]}
        rotation={[0, Math.PI / 2, 0]}
      >
        <FeatureFace feature={FEATURES[1]} />
      </HtmlFace>

      {/* Back: feature 3 */}
      <HtmlFace
        position={[0, 0, -DEPTH / 2 - 0.01]}
        rotation={[0, Math.PI, 0]}
      >
        <FeatureFace feature={FEATURES[2]} />
      </HtmlFace>

      {/* Left: feature 4 */}
      <HtmlFace
        position={[-FACE_W / 2 - 0.01, 0, 0]}
        rotation={[0, -Math.PI / 2, 0]}
      >
        <FeatureFace feature={FEATURES[3]} />
      </HtmlFace>

      {/* Top: graphs deck */}
      <HtmlFace
        position={[0, FACE_H / 2 + 0.01, 0]}
        rotation={[-Math.PI / 2, 0, 0]}
      >
        <GraphsFace />
      </HtmlFace>
    </group>
  );
}

function HtmlFace({
  children,
  position,
  rotation,
}: {
  children: React.ReactNode;
  position: [number, number, number];
  rotation: [number, number, number];
}) {
  return (
    <group position={position} rotation={rotation}>
      <mesh>
        <planeGeometry args={[FACE_W, FACE_H]} />
        <meshStandardMaterial
          color="#0b0e08"
          roughness={0.9}
          metalness={0.08}
          transparent
          opacity={0.88}
          side={THREE.DoubleSide}
        />
      </mesh>

      <Html
        transform
        center
        scale={HTML_SCALE}
        zIndexRange={[20, 0]}
        className="spinHtml"
      >
        {children}
      </Html>
    </group>
  );
}

function FeatureFace({ feature }: { feature: Feature }) {
  return (
    <article className="spinFace featureFace">
      <div className="featureCopy">
        <div className="featureNumber">{feature.number}.</div>
        <h3>{feature.title}</h3>
        <div className="titleRule" />
        <p>{feature.body}</p>
      </div>

      <div className="featureIconWrap">
        <FeatureGlyph icon={feature.icon} />
      </div>
    </article>
  );
}

function GraphsFace() {
  return (
    <article className="spinFace graphsFace">
      <header className="graphsHeader">
        <p>Graphs</p>
        <h3>Evidence layer</h3>
        <span>Data-backed checks from replayable simulations.</span>
      </header>

      <div className="miniGraphGrid">
        <MiniChart
          title="Append-Only Log Integrity"
          subtitle="Monotonic event growth"
          kind="line"
        />
        <MiniChart
          title="Memory Rebuild Latency"
          subtitle="Fresh build per decision"
          kind="bars"
        />
        <MiniChart
          title="Audit Verification"
          subtitle="Replay match rate"
          kind="flat"
        />
      </div>
    </article>
  );
}

function FeatureGlyph({ icon }: { icon: FeatureIcon }) {
  if (icon === "log") {
    return (
      <div className="glyph glyphLog" aria-hidden="true">
        {[0, 1, 2, 3, 4].map((item) => (
          <span key={item} />
        ))}
        <em>•••</em>
      </div>
    );
  }

  if (icon === "refresh") {
    return (
      <svg className="glyphSvg" viewBox="0 0 180 180" aria-hidden="true">
        <path
          d="M35 96a58 58 0 0 1 92-47"
          fill="none"
          stroke="currentColor"
          strokeWidth="5"
          strokeLinecap="round"
        />
        <path
          d="M126 32l2 25-23-8"
          fill="none"
          stroke="currentColor"
          strokeWidth="5"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
        <path
          d="M145 84a58 58 0 0 1-92 47"
          fill="none"
          stroke="currentColor"
          strokeWidth="5"
          strokeLinecap="round"
        />
        <path
          d="M54 148l-2-25 23 8"
          fill="none"
          stroke="currentColor"
          strokeWidth="5"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
        {[0, 1, 2, 3, 4, 5, 6, 7, 8].map((cell) => {
          const x = 60 + (cell % 3) * 22;
          const y = 63 + Math.floor(cell / 3) * 22;
          return (
            <rect
              key={cell}
              x={x}
              y={y}
              width="16"
              height="16"
              rx="2"
              className={cell === 5 ? "hotCell" : ""}
            />
          );
        })}
      </svg>
    );
  }

  if (icon === "certificate") {
    return (
      <svg className="glyphSvg" viewBox="0 0 180 180" aria-hidden="true">
        <path
          d="M90 20l36 20v41l-36 21-36-21V40z"
          fill="rgba(220,226,80,.2)"
          stroke="currentColor"
          strokeWidth="4"
        />
        <path
          d="M54 40l36 21 36-21M90 61v41"
          fill="none"
          stroke="currentColor"
          strokeWidth="3"
          opacity=".8"
        />
        <path
          d="M90 104v22M48 126h84M48 126v18M90 126v18M132 126v18"
          fill="none"
          stroke="currentColor"
          strokeWidth="4"
          strokeLinecap="round"
        />
        {[48, 90, 132].map((x) => (
          <circle
            key={x}
            cx={x}
            cy="148"
            r="11"
            fill="rgba(220,226,80,.16)"
            stroke="currentColor"
            strokeWidth="4"
          />
        ))}
      </svg>
    );
  }

  return (
    <svg className="glyphSvg" viewBox="0 0 180 180" aria-hidden="true">
      <path
        d="M54 28h62l14 14v110l-10-7-10 7-10-7-10 7-10-7-10 7-10-7-10 7V28z"
        fill="rgba(220,226,80,.08)"
        stroke="currentColor"
        strokeWidth="4"
        strokeLinejoin="round"
      />
      <path
        d="M116 28v24h24"
        fill="none"
        stroke="currentColor"
        strokeWidth="4"
        strokeLinejoin="round"
      />
      <circle
        cx="88"
        cy="66"
        r="22"
        fill="rgba(220,226,80,.13)"
        stroke="currentColor"
        strokeWidth="4"
      />
      <path
        d="M76 66l8 8 17-19"
        fill="none"
        stroke="currentColor"
        strokeWidth="5"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      <path
        d="M69 105h48M69 124h38"
        fill="none"
        stroke="currentColor"
        strokeWidth="5"
        strokeLinecap="round"
        opacity=".72"
      />
    </svg>
  );
}

function MiniChart({
  title,
  subtitle,
  kind,
}: {
  title: string;
  subtitle: string;
  kind: "line" | "bars" | "flat";
}) {
  return (
    <section className="miniChart">
      <h4>{title}</h4>
      <p>{subtitle}</p>

      <svg viewBox="0 0 260 130" role="img" aria-label={title}>
        <g className="gridLines">
          {[25, 50, 75, 100].map((y) => (
            <line key={y} x1="18" x2="248" y1={y} y2={y} />
          ))}
          {[60, 110, 160, 210].map((x) => (
            <line key={x} x1={x} x2={x} y1="16" y2="112" />
          ))}
        </g>

        {kind === "line" && (
          <>
            <path
              className="area"
              d="M18 110 L46 96 L76 85 L104 76 L132 62 L160 52 L190 37 L220 29 L248 15 L248 112 L18 112 Z"
            />
            <path
              className="chartLine"
              d="M18 110 L46 96 L76 85 L104 76 L132 62 L160 52 L190 37 L220 29 L248 15"
            />
          </>
        )}

        {kind === "bars" &&
          [44, 70, 58, 82, 64, 93, 75, 62, 86, 55, 72, 95].map(
            (height, index) => (
              <rect
                key={index}
                className="chartBar"
                x={25 + index * 18}
                y={112 - height}
                width="9"
                height={height}
                rx="2"
              />
            ),
          )}

        {kind === "flat" && (
          <>
            <path
              className="area"
              d="M18 28 L52 30 L86 27 L120 29 L154 27 L188 29 L222 28 L248 29 L248 112 L18 112 Z"
            />
            <path
              className="chartLine"
              d="M18 28 L52 30 L86 27 L120 29 L154 27 L188 29 L222 28 L248 29"
            />
          </>
        )}
      </svg>
    </section>
  );
}
