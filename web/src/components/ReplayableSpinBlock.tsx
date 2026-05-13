"use client";

import React, { useCallback, useEffect, useMemo, useRef } from "react";
import { Canvas, useFrame } from "@react-three/fiber";
import { Edges } from "@react-three/drei";
import * as THREE from "three";
import "./ReplayableSpinBlock.css";

const GRAPH_INDEX = 4;

const FACE_W = 5.6;
const FACE_H = 3.9;
const DEPTH = 5.6;

type FeatureIcon = "log" | "refresh" | "certificate" | "receipt";

type Feature = {
  number: string;
  title: string;
  icon: FeatureIcon;
};

const FEATURES: Feature[] = [
  {
    number: "1",
    title: "Append-only logs don’t hallucinate",
    icon: "log",
  },
  {
    number: "2",
    title: "Memory rebuilt at decision time",
    icon: "refresh",
  },
  {
    number: "3",
    title: "Content-addressed audit certificates",
    icon: "certificate",
  },
  {
    number: "4",
    title: "Decisions with receipts",
    icon: "receipt",
  },
];

const PANEL_COUNT = FEATURES.length + 1;

type ReplayableSpinBlockProps = {
  activeIndex: number;
  onActiveIndexChange: (index: number) => void;
};

export default function ReplayableSpinBlock({
  activeIndex,
  onActiveIndexChange,
}: ReplayableSpinBlockProps) {

  const dragStartX = useRef<number | null>(null);

  const currentIndex = Math.min(Math.max(activeIndex, 0), PANEL_COUNT - 1);

  const goTo = useCallback((index: number) => {
    const boundedIndex = (index + PANEL_COUNT) % PANEL_COUNT;
    onActiveIndexChange(boundedIndex);
  }, [onActiveIndexChange]);

  const next = useCallback(() => {
    const nextIndex = (currentIndex + 1) % PANEL_COUNT;
    goTo(nextIndex);
  }, [currentIndex, goTo]);

  const previous = useCallback(() => {
    const previousIndex = (currentIndex + PANEL_COUNT - 1) % PANEL_COUNT;
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

        <SpinBlock activeIndex={currentIndex} />
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
  activeIndex,
}: {
  activeIndex: number;
}) {
  const groupRef = useRef<THREE.Group | null>(null);
  const showGraphs = activeIndex === GRAPH_INDEX;
  const featureIndex = showGraphs ? 0 : activeIndex;

  useFrame((_, delta) => {
    if (!groupRef.current) return;

    // Each of the 4 features lives on a distinct face of the cube,
    // rotated -π/2 apart on Y. Graphs lives on top (X-axis tilt).
    const targetY = showGraphs ? 0 : -featureIndex * (Math.PI / 2);
    const targetX = showGraphs ? Math.PI / 2 : 0;
    const frameDelta = Math.min(delta, 1 / 30);

    groupRef.current.rotation.y = THREE.MathUtils.damp(
      groupRef.current.rotation.y,
      targetY,
      5.5,
      frameDelta,
    );
    groupRef.current.rotation.x = THREE.MathUtils.damp(
      groupRef.current.rotation.x,
      targetX,
      5.5,
      frameDelta,
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
        />
        <Edges scale={1.002} color="#d9df4f" threshold={15} linewidth={1} />
      </mesh>

      {/* Front: feature 1 */}
      <TextureFace
        position={[0, 0, DEPTH / 2 + 0.01]}
        rotation={[0, 0, 0]}
        texture={<FeatureFaceTexture feature={FEATURES[0]} />}
      />

      {/* Right: feature 2 */}
      <TextureFace
        position={[FACE_W / 2 + 0.01, 0, 0]}
        rotation={[0, Math.PI / 2, 0]}
        texture={<FeatureFaceTexture feature={FEATURES[1]} />}
      />

      {/* Back: feature 3 */}
      <TextureFace
        position={[0, 0, -DEPTH / 2 - 0.01]}
        rotation={[0, Math.PI, 0]}
        texture={<FeatureFaceTexture feature={FEATURES[2]} />}
      />

      {/* Left: feature 4 */}
      <TextureFace
        position={[-FACE_W / 2 - 0.01, 0, 0]}
        rotation={[0, -Math.PI / 2, 0]}
        texture={<FeatureFaceTexture feature={FEATURES[3]} />}
      />

      {/* Top: graphs deck */}
      <TextureFace
        position={[0, FACE_H / 2 + 0.01, 0]}
        rotation={[-Math.PI / 2, 0, 0]}
        texture={<GraphsFaceTexture />}
      />
    </group>
  );
}

function TextureFace({
  position,
  rotation,
  texture,
}: {
  position: [number, number, number];
  rotation: [number, number, number];
  texture: React.ReactElement;
}) {
  return (
    <group position={position} rotation={rotation}>
      <mesh>
        <planeGeometry args={[FACE_W, FACE_H]} />
        {texture}
      </mesh>
    </group>
  );
}

function FeatureFaceTexture({ feature }: { feature: Feature }) {
  const texture = useFaceTexture(() => createFeatureTexture(feature), [feature]);

  return (
    <meshStandardMaterial
      map={texture}
      roughness={0.9}
      metalness={0.08}
      side={THREE.FrontSide}
    />
  );
}

function GraphsFaceTexture() {
  const texture = useFaceTexture(createGraphsTexture, []);

  return (
    <meshStandardMaterial
      map={texture}
      roughness={0.9}
      metalness={0.08}
      side={THREE.FrontSide}
    />
  );
}

function useFaceTexture(factory: () => THREE.CanvasTexture, deps: React.DependencyList) {
  const texture = useMemo(factory, deps);

  useEffect(() => {
    return () => texture.dispose();
  }, [texture]);

  return texture;
}

const TEXTURE_W = 1120;
const TEXTURE_H = 780;
const TEXTURE_PADDING = 96;

function createFeatureTexture(feature: Feature) {
  const { canvas, ctx } = createFaceCanvas();

  drawFacePanel(ctx);
  drawDivider(ctx);
  drawFeatureCopy(ctx, feature);
  drawFeatureIcon(ctx, feature.icon);

  return createTexture(canvas);
}

function createGraphsTexture() {
  const { canvas, ctx } = createFaceCanvas();

  drawFacePanel(ctx);

  ctx.fillStyle = "#dce452";
  ctx.font = "700 34px ui-monospace, Menlo, Consolas, monospace";
  ctx.letterSpacing = "4px";
  ctx.fillText("GRAPHS", TEXTURE_PADDING, 150);

  ctx.letterSpacing = "0px";
  ctx.font = "800 66px ui-monospace, Menlo, Consolas, monospace";
  wrapCanvasText(ctx, "Evidence layer", TEXTURE_PADDING, 235, 300, 68);

  ctx.fillStyle = "rgba(246, 246, 232, 0.68)";
  ctx.font = "400 28px Inter, system-ui, sans-serif";
  wrapCanvasText(
    ctx,
    "Data-backed checks from replayable simulations.",
    TEXTURE_PADDING,
    420,
    310,
    42,
  );

  drawMiniGraph(ctx, 490, 94, "Append-Only Log Integrity", "line");
  drawMiniGraph(ctx, 490, 288, "Memory Rebuild Latency", "bars");
  drawMiniGraph(ctx, 490, 482, "Audit Verification", "flat");

  return createTexture(canvas);
}

function createFaceCanvas() {
  const canvas = document.createElement("canvas");
  canvas.width = TEXTURE_W;
  canvas.height = TEXTURE_H;

  const ctx = canvas.getContext("2d");
  if (!ctx) {
    throw new Error("Unable to create cube face texture canvas");
  }

  return { canvas, ctx };
}

function createTexture(canvas: HTMLCanvasElement) {
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.anisotropy = 8;
  texture.needsUpdate = true;
  return texture;
}

function drawFacePanel(ctx: CanvasRenderingContext2D) {
  const gradient = ctx.createLinearGradient(0, 0, TEXTURE_W, TEXTURE_H);
  gradient.addColorStop(0, "#15190e");
  gradient.addColorStop(0.42, "#090b07");
  gradient.addColorStop(1, "#050604");
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, TEXTURE_W, TEXTURE_H);

  const glow = ctx.createRadialGradient(130, 60, 0, 130, 60, 440);
  glow.addColorStop(0, "rgba(220, 228, 82, 0.18)");
  glow.addColorStop(1, "rgba(220, 228, 82, 0)");
  ctx.fillStyle = glow;
  ctx.fillRect(0, 0, TEXTURE_W, TEXTURE_H);

  ctx.strokeStyle = "rgba(220, 228, 82, 0.52)";
  ctx.lineWidth = 2;
  roundedRect(ctx, 20, 20, TEXTURE_W - 40, TEXTURE_H - 40, 52);
  ctx.stroke();

  ctx.strokeStyle = "rgba(255, 255, 255, 0.04)";
  ctx.lineWidth = 2;
  roundedRect(ctx, 32, 32, TEXTURE_W - 64, TEXTURE_H - 64, 44);
  ctx.stroke();
}

function drawDivider(ctx: CanvasRenderingContext2D) {
  ctx.strokeStyle = "rgba(220, 228, 82, 0.3)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(700, 120);
  ctx.lineTo(700, 660);
  ctx.stroke();
}

function drawFeatureCopy(ctx: CanvasRenderingContext2D, feature: Feature) {
  ctx.fillStyle = "#dce452";
  ctx.font = "800 116px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillText(`${feature.number}.`, TEXTURE_PADDING, 280);

  ctx.font = "800 58px ui-monospace, Menlo, Consolas, monospace";
  wrapCanvasText(ctx, feature.title, TEXTURE_PADDING, 370, 460, 62);
}

function drawFeatureIcon(ctx: CanvasRenderingContext2D, icon: FeatureIcon) {
  ctx.save();
  ctx.translate(840, 390);

  const glow = ctx.createRadialGradient(0, 0, 0, 0, 0, 230);
  glow.addColorStop(0, "rgba(220, 228, 82, 0.16)");
  glow.addColorStop(1, "rgba(220, 228, 82, 0)");
  ctx.fillStyle = glow;
  ctx.fillRect(-230, -230, 460, 460);

  ctx.strokeStyle = "rgba(220, 228, 82, 0.34)";
  ctx.lineWidth = 2;
  roundedRect(ctx, -150, -150, 300, 300, 18);
  ctx.stroke();

  ctx.fillStyle = "rgba(220, 228, 82, 0.82)";
  ctx.strokeStyle = "#dce452";
  ctx.lineWidth = 10;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  if (icon === "log") {
    for (let row = 0; row < 5; row += 1) {
      ctx.beginPath();
      ctx.arc(-105, -82 + row * 42, 9, 0, Math.PI * 2);
      ctx.fill();
      roundedRect(ctx, -70, -98 + row * 42, 145, 24, 4);
      ctx.fillStyle = "rgba(220, 228, 82, 0.46)";
      ctx.fill();
      ctx.fillStyle = "rgba(220, 228, 82, 0.82)";
    }
    ctx.font = "800 44px ui-monospace, Menlo, Consolas, monospace";
    ctx.fillText("...", -34, 124);
  } else if (icon === "refresh") {
    drawGrid(ctx, -105, -92, 52, 3, 3);
    ctx.beginPath();
    ctx.moveTo(-34, 104);
    ctx.lineTo(0, 132);
    ctx.lineTo(34, 104);
    ctx.stroke();
  } else if (icon === "certificate") {
    ctx.beginPath();
    ctx.moveTo(0, -130);
    ctx.lineTo(70, -90);
    ctx.lineTo(70, -12);
    ctx.lineTo(0, 30);
    ctx.lineTo(-70, -12);
    ctx.lineTo(-70, -90);
    ctx.closePath();
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, 30);
    ctx.lineTo(0, 82);
    ctx.moveTo(-86, 82);
    ctx.lineTo(86, 82);
    ctx.moveTo(-86, 82);
    ctx.lineTo(-86, 124);
    ctx.moveTo(0, 82);
    ctx.lineTo(0, 124);
    ctx.moveTo(86, 82);
    ctx.lineTo(86, 124);
    ctx.stroke();
    [-86, 0, 86].forEach((x) => {
      ctx.beginPath();
      ctx.arc(x, 134, 22, 0, Math.PI * 2);
      ctx.stroke();
    });
  } else {
    ctx.beginPath();
    ctx.moveTo(-76, -126);
    ctx.lineTo(46, -126);
    ctx.lineTo(92, -80);
    ctx.lineTo(92, 130);
    ctx.lineTo(72, 112);
    ctx.lineTo(48, 130);
    ctx.lineTo(24, 112);
    ctx.lineTo(0, 130);
    ctx.lineTo(-24, 112);
    ctx.lineTo(-48, 130);
    ctx.lineTo(-76, 112);
    ctx.closePath();
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(0, -40, 46, 0, Math.PI * 2);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(-20, -40);
    ctx.lineTo(-4, -22);
    ctx.lineTo(32, -64);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(-44, 50);
    ctx.lineTo(52, 50);
    ctx.moveTo(-44, 92);
    ctx.lineTo(28, 92);
    ctx.stroke();
  }

  ctx.restore();
}

function drawGrid(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  cell: number,
  cols: number,
  rows: number,
) {
  for (let row = 0; row < rows; row += 1) {
    for (let col = 0; col < cols; col += 1) {
      ctx.fillStyle = row === 0 && col === 2 ? "#dce452" : "rgba(220, 228, 82, 0.34)";
      ctx.fillRect(x + col * (cell + 10), y + row * (cell + 10), cell, cell);
    }
  }
}

function drawMiniGraph(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  title: string,
  kind: "line" | "bars" | "flat",
) {
  roundedRect(ctx, x, y, 520, 154, 22);
  ctx.fillStyle = "rgba(5, 7, 5, 0.5)";
  ctx.fill();
  ctx.strokeStyle = "rgba(220, 228, 82, 0.28)";
  ctx.lineWidth = 2;
  ctx.stroke();

  ctx.fillStyle = "#dce452";
  ctx.font = "700 24px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillText(title, x + 28, y + 42);

  ctx.strokeStyle = "rgba(246, 246, 232, 0.12)";
  ctx.lineWidth = 1;
  for (let i = 0; i < 4; i += 1) {
    const lineY = y + 68 + i * 22;
    ctx.beginPath();
    ctx.moveTo(x + 28, lineY);
    ctx.lineTo(x + 492, lineY);
    ctx.stroke();
  }

  ctx.strokeStyle = "#dce452";
  ctx.fillStyle = "rgba(220, 228, 82, 0.78)";
  ctx.lineWidth = 5;
  ctx.beginPath();
  if (kind === "bars") {
    [42, 68, 52, 86, 64, 94, 74, 58, 84, 50].forEach((height, index) => {
      ctx.fillRect(x + 44 + index * 42, y + 130 - height, 18, height);
    });
  } else {
    const points =
      kind === "line"
        ? [128, 116, 106, 92, 84, 70, 58, 44, 34]
        : [50, 52, 49, 51, 49, 52, 50, 51, 50];
    points.forEach((pointY, index) => {
      const px = x + 34 + index * 54;
      const py = y + pointY;
      if (index === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    });
    ctx.stroke();
  }
}

function wrapCanvasText(
  ctx: CanvasRenderingContext2D,
  text: string,
  x: number,
  y: number,
  maxWidth: number,
  lineHeight: number,
) {
  const words = text.split(" ");
  let line = "";
  let cursorY = y;

  words.forEach((word) => {
    const testLine = line ? `${line} ${word}` : word;
    if (ctx.measureText(testLine).width > maxWidth && line) {
      ctx.fillText(line, x, cursorY);
      line = word;
      cursorY += lineHeight;
    } else {
      line = testLine;
    }
  });

  if (line) {
    ctx.fillText(line, x, cursorY);
  }
}

function roundedRect(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  radius: number,
) {
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.lineTo(x + width - radius, y);
  ctx.quadraticCurveTo(x + width, y, x + width, y + radius);
  ctx.lineTo(x + width, y + height - radius);
  ctx.quadraticCurveTo(x + width, y + height, x + width - radius, y + height);
  ctx.lineTo(x + radius, y + height);
  ctx.quadraticCurveTo(x, y + height, x, y + height - radius);
  ctx.lineTo(x, y + radius);
  ctx.quadraticCurveTo(x, y, x + radius, y);
  ctx.closePath();
}

function FeatureGlyph({ icon }: { icon: FeatureIcon }) {
  // Sprite from public/art/cubeart.png — a vertical strip of 4 glyphs.
  // Each glyph occupies ~25% of the image height (4 panels stacked).
  const SPRITE_INDEX: Record<FeatureIcon, number> = {
    log: 0,
    refresh: 1,
    certificate: 2,
    receipt: 3,
  };
  const idx = SPRITE_INDEX[icon];
  const positionY = idx * (100 / 3); // 0%, 33.33%, 66.66%, 100%

  return (
    <div
      className="glyphSprite"
      role="img"
      aria-label={icon}
      style={{
        backgroundImage: "url(/vet/art/cubeart.png)",
        backgroundPositionY: `${positionY}%`,
      }}
    />
  );
}

function _LegacyFeatureGlyph({ icon }: { icon: FeatureIcon }) {
  // Kept only for type-export consistency; the live glyph is FeatureGlyph
  // above which uses cubeart.png as a sprite. Safe to delete on next pass.
  if (icon === "log") {
    return null;
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
