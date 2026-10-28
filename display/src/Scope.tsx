import { useEffect, useRef, type PointerEvent } from "react";

import { CHEVRON, PILLAR, PILLAR_CUT } from "./components/Logo";
import { bearingDeg, headingDeg, pad3, range, speed } from "./geometry";
import type { TrackState } from "./shared/protocol";
import type { Feed } from "./useTrackFeed";

export interface Layers {
  detections: boolean;
  trails: boolean;
  leaders: boolean;
  uncertainty: boolean;
  dataBlocks: boolean;
  tentative: boolean;
  sweep: boolean;
}

/** Screen space the floating panels cover; the sensor is centred in what's left. */
export interface Insets {
  top: number;
  right: number;
  bottom: number;
  left: number;
}

interface ScopeProps {
  feed: Feed;
  layers: Layers;
  insets: Insets;
  selectedId: number | undefined;
  onSelect: (id: number | undefined) => void;
}

interface View {
  cx: number; // world point at the centre of the visible area, metres
  cy: number;
  scale: number; // pixels per metre; 0 = fit on next frame
}

const MONO = "'JetBrains Mono Variable', ui-monospace, Consolas, monospace";

const C = {
  bgInner: "#0b1b22",
  bgOuter: "#03070a",
  ringMajor: "rgba(96, 170, 184, 0.16)",
  ringMinor: "rgba(96, 170, 184, 0.07)",
  spoke: "rgba(96, 170, 184, 0.06)",
  ringLabel: "rgba(138, 178, 190, 0.55)",
  bezel: "rgba(110, 190, 205, 0.28)",
  bezelLabel: "rgba(160, 200, 210, 0.75)",
  sensor: "#a9c8d2",
  detection: "214, 236, 240",
  sweep: "62, 230, 196",
  confirmed: "#3ee6c4",
  coasting: "#f5b443",
  tentative: "#7f909b",
  selected: "#ffffff",
  blockBg: "rgba(4, 11, 15, 0.78)",
  blockText: "rgba(200, 222, 230, 0.85)",
  blockId: "rgba(140, 170, 182, 0.55)",
};

const DEFAULT_SPAN_M = 46_000;
const BEZEL_RANGE_M = 25_000;
const MIN_BEZEL_PX = 130;
const LEADER_SECONDS = 60;
const HISTORY_DOTS = 14;
const NICE_STEPS = [250, 500, 1000, 2000, 2500, 5000, 10_000, 20_000, 50_000, 100_000];

const statusColor = (t: TrackState): string =>
  t.status === "confirmed" ? C.confirmed : t.status === "coasting" ? C.coasting : C.tentative;

const reducedMotion = () => window.matchMedia("(prefers-reduced-motion: reduce)").matches;

/** Plan-position-indicator (PPI) scope: north up, sensor at the origin. */
export function Scope({ feed, layers, insets, selectedId, onSelect }: ScopeProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const cursorRef = useRef<HTMLSpanElement>(null);
  const scaleRef = useRef<HTMLSpanElement>(null);
  const view = useRef<View>({ cx: 0, cy: 0, scale: 0 });
  const hoverId = useRef<number | undefined>(undefined);
  // An animated move of the view: centre always, scale too when s0/s1 are set.
  const glide = useRef<{
    x0: number;
    y0: number;
    x1: number;
    y1: number;
    s0?: number;
    s1?: number;
    t0: number;
  }>(undefined);

  // The draw loop reads the newest props through a ref instead of restarting each scan.
  const props = useRef({ feed, layers, insets, selectedId });
  props.current = { feed, layers, insets, selectedId };

  /** Screen position of the visible-area centre. */
  const centre = (w: number, h: number) => {
    const i = props.current.insets;
    return { x: i.left + (w - i.left - i.right) / 2, y: i.top + (h - i.top - i.bottom) / 2 };
  };

  const toWorld = (sx: number, sy: number) => {
    const canvas = canvasRef.current!;
    const v = view.current;
    const c = centre(canvas.clientWidth, canvas.clientHeight);
    return { x: v.cx + (sx - c.x) / v.scale, y: v.cy - (sy - c.y) / v.scale };
  };

  useEffect(() => {
    const canvas = canvasRef.current!;
    const ctx = canvas.getContext("2d")!;
    let raf = 0;
    let lastStep = 0;

    const draw = () => {
      raf = requestAnimationFrame(draw);
      const dpr = window.devicePixelRatio || 1;
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
        canvas.width = Math.round(w * dpr);
        canvas.height = Math.round(h * dpr);
      }
      const i = props.current.insets;
      if (view.current.scale === 0) {
        const span = Math.min(w - i.left - i.right, h - i.top - i.bottom);
        view.current = { cx: 0, cy: 0, scale: Math.max(span, 200) / DEFAULT_SPAN_M };
        glide.current = undefined;
      }
      const g = glide.current;
      if (g) {
        const k = reducedMotion() ? 1 : Math.min((performance.now() - g.t0) / 550, 1);
        const ease = 1 - Math.pow(1 - k, 3);
        view.current.cx = g.x0 + (g.x1 - g.x0) * ease;
        view.current.cy = g.y0 + (g.y1 - g.y0) * ease;
        if (g.s0 !== undefined && g.s1 !== undefined) {
          // Interpolate zoom geometrically so it feels even in and out.
          view.current.scale = g.s0 * Math.pow(g.s1 / g.s0, ease);
        }
        if (k === 1) glide.current = undefined;
      }
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const step = render(ctx, w, h, centre(w, h), view.current, props.current, hoverId.current);
      if (step !== lastStep && scaleRef.current) {
        lastStep = step;
        scaleRef.current.textContent = step >= 1000 ? `${step / 1000} km` : `${step} m`;
      }
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  }, []);

  // Selecting a track that's off screen (e.g. from the list) glides the view to it.
  useEffect(() => {
    const canvas = canvasRef.current;
    const t = feed.frame?.tracks.find((tr) => tr.id === selectedId);
    if (!canvas || !t || view.current.scale === 0) return;
    const v = view.current;
    const i = insets;
    const w = canvas.clientWidth;
    const h = canvas.clientHeight;
    const c = centre(w, h);
    const px = c.x + (t.x - v.cx) * v.scale;
    const py = c.y - (t.y - v.cy) * v.scale;
    const margin = 48;
    const inView =
      px > i.left + margin && px < w - i.right - margin && py > i.top + margin && py < h - i.bottom - margin;
    if (!inView) glide.current = { x0: v.cx, y0: v.cy, x1: t.x, y1: t.y, t0: performance.now() };
    // Deliberately keyed on the selection only, not on every scan.
  }, [selectedId]);

  // ------------------------------------------------------------ interaction

  const pickTrack = (sx: number, sy: number): number | undefined => {
    const { feed: f, layers: l } = props.current;
    const p = toWorld(sx, sy);
    let best: number | undefined;
    let bestDist = 14 / view.current.scale;
    for (const t of f.frame?.tracks ?? []) {
      if (t.status === "tentative" && !l.tentative) continue;
      const d = Math.hypot(t.x - p.x, t.y - p.y);
      if (d < bestDist) {
        bestDist = d;
        best = t.id;
      }
    }
    return best;
  };

  const zoomAt = (sx: number, sy: number, factor: number) => {
    glide.current = undefined;
    const before = toWorld(sx, sy);
    view.current.scale = Math.min(Math.max(view.current.scale * factor, 0.0004), 2);
    const after = toWorld(sx, sy);
    view.current.cx += before.x - after.x;
    view.current.cy += before.y - after.y;
  };

  const zoomCentre = (factor: number) => {
    const canvas = canvasRef.current!;
    const c = centre(canvas.clientWidth, canvas.clientHeight);
    zoomAt(c.x, c.y, factor);
  };

  /** Size of the area not covered by floating panels, in px. */
  const visibleSize = () => {
    const canvas = canvasRef.current!;
    const i = props.current.insets;
    return {
      w: Math.max(canvas.clientWidth - i.left - i.right, 200),
      h: Math.max(canvas.clientHeight - i.top - i.bottom, 200),
    };
  };

  const glideTo = (cx: number, cy: number, scale: number) => {
    const v = view.current;
    if (v.scale === 0) return;
    glide.current = { x0: v.cx, y0: v.cy, x1: cx, y1: cy, s0: v.scale, s1: scale, t0: performance.now() };
  };

  /** Default view: sensor centred, the standard surveillance area filling the screen. */
  const resetView = () => {
    const { w, h } = visibleSize();
    glideTo(0, 0, Math.min(w, h) / DEFAULT_SPAN_M);
  };

  /** Frame everything on the radar: the sensor, every shown track and the latest detections. */
  const fitAll = () => {
    const { feed: f, layers: l } = props.current;
    const xs = [0];
    const ys = [0];
    for (const t of f.frame?.tracks ?? []) {
      if (t.status === "tentative" && !l.tentative) continue;
      xs.push(t.x);
      ys.push(t.y);
    }
    if (l.detections) {
      for (const d of f.detectionHistory.at(-1) ?? []) {
        xs.push(d.x);
        ys.push(d.y);
      }
    }
    const minX = Math.min(...xs);
    const maxX = Math.max(...xs);
    const minY = Math.min(...ys);
    const maxY = Math.max(...ys);
    const { w, h } = visibleSize();
    const pad = 70; // px of breathing room, so data blocks stay on screen
    const MIN_SPAN_M = 8000; // don't zoom in absurdly far on a single track
    const spanX = Math.max(maxX - minX, MIN_SPAN_M);
    const spanY = Math.max(maxY - minY, MIN_SPAN_M);
    const scale = Math.min((w - 2 * pad) / spanX, (h - 2 * pad) / spanY);
    glideTo((minX + maxX) / 2, (minY + maxY) / 2, Math.min(Math.max(scale, 0.0004), 2));
  };

  // F frames everything, R (or Home) resets, unless the user is typing somewhere.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      if (e.target instanceof HTMLElement && e.target.closest("input, textarea, select")) return;
      if (e.key === "f" || e.key === "F") fitAll();
      else if (e.key === "r" || e.key === "R" || e.key === "Home") resetView();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const drag = useRef<{ x: number; y: number; moved: boolean } | undefined>(undefined);

  const onPointerDown = (e: PointerEvent<HTMLCanvasElement>) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { x: e.nativeEvent.offsetX, y: e.nativeEvent.offsetY, moved: false };
  };

  const onPointerMove = (e: PointerEvent<HTMLCanvasElement>) => {
    const { offsetX: sx, offsetY: sy } = e.nativeEvent;
    const p = toWorld(sx, sy);
    if (cursorRef.current) {
      cursorRef.current.textContent = `${(range(p.x, p.y) / 1000).toFixed(2)} km  ${pad3(bearingDeg(p.x, p.y))}°`;
    }
    const d = drag.current;
    if (d) {
      const dx = sx - d.x;
      const dy = sy - d.y;
      if (!d.moved && Math.hypot(dx, dy) < 4) return;
      d.moved = true;
      glide.current = undefined;
      view.current.cx -= dx / view.current.scale;
      view.current.cy += dy / view.current.scale;
      d.x = sx;
      d.y = sy;
      e.currentTarget.style.cursor = "grabbing";
      return;
    }
    hoverId.current = pickTrack(sx, sy);
    e.currentTarget.style.cursor = hoverId.current !== undefined ? "pointer" : "crosshair";
  };

  const onPointerUp = (e: PointerEvent<HTMLCanvasElement>) => {
    const d = drag.current;
    drag.current = undefined;
    e.currentTarget.style.cursor = "crosshair";
    if (!d || d.moved) return;
    onSelect(pickTrack(e.nativeEvent.offsetX, e.nativeEvent.offsetY));
  };

  useEffect(() => {
    const canvas = canvasRef.current!;
    // React's onWheel is passive, so preventDefault needs a native listener.
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      zoomAt(e.offsetX, e.offsetY, Math.exp(-e.deltaY * 0.0015));
    };
    canvas.addEventListener("wheel", onWheel, { passive: false });
    return () => canvas.removeEventListener("wheel", onWheel);
  }, []);

  return (
    <div className="scope">
      <canvas
        ref={canvasRef}
        aria-label="Radar scope showing tracks and detections"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerLeave={() => {
          hoverId.current = undefined;
          if (cursorRef.current) cursorRef.current.textContent = "—";
        }}
        onDoubleClick={resetView}
      />
      {/* Bottom-left sits below the display card, so it ignores the left inset. */}
      <div className="scope-readout glass" style={{ left: 16, bottom: insets.bottom + 16 }}>
        <div>
          <span className="readout-key">CURSOR</span>
          <span ref={cursorRef} className="readout-val">—</span>
        </div>
        <div>
          <span className="readout-key">RINGS</span>
          <span ref={scaleRef} className="readout-val">—</span>
        </div>
      </div>
      <div className="zoom glass" style={{ right: insets.right + 16, bottom: insets.bottom + 16 }}>
        <button type="button" className="fit" onClick={fitAll} title="Fit everything on the radar (F)">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <path d="M2 5.5V2h3.5M10.5 2H14v3.5M14 10.5V14h-3.5M5.5 14H2v-3.5" />
            <circle cx="8" cy="8" r="1.6" />
          </svg>
          Show all
        </button>
        <span className="zoom-divider" aria-hidden="true" />
        <button type="button" onClick={() => zoomCentre(1.4)} aria-label="Zoom in" title="Zoom in (scroll)">
          <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 3v10M3 8h10" /></svg>
        </button>
        <button type="button" onClick={() => zoomCentre(1 / 1.4)} aria-label="Zoom out" title="Zoom out (scroll)">
          <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 8h10" /></svg>
        </button>
        <button type="button" onClick={resetView} aria-label="Reset view" title="Centre on the sensor (R or double-click)">
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <circle cx="8" cy="8" r="4.5" />
            <path d="M8 1.5v3M8 11.5v3M1.5 8h3M11.5 8h3" />
          </svg>
        </button>
      </div>
    </div>
  );
}

// ------------------------------------------------------------------ rendering

function render(
  ctx: CanvasRenderingContext2D,
  w: number,
  h: number,
  centre: { x: number; y: number },
  v: View,
  {
    feed,
    layers,
    insets,
    selectedId,
  }: { feed: Feed; layers: Layers; insets: Insets; selectedId: number | undefined },
  hoverId: number | undefined,
): number {
  const now = performance.now();
  const sx = (x: number) => centre.x + (x - v.cx) * v.scale;
  const sy = (y: number) => centre.y - (y - v.cy) * v.scale;
  const ox = sx(0);
  const oy = sy(0);

  // Farthest visible distance from the sensor decides how far rings and spokes go.
  const reach = Math.max(
    Math.hypot(ox, oy),
    Math.hypot(w - ox, oy),
    Math.hypot(ox, h - oy),
    Math.hypot(w - ox, h - oy),
  );

  // Background: a soft pool of light around the sensor.
  const bg = ctx.createRadialGradient(ox, oy, 0, ox, oy, Math.max(w, h) * 0.85);
  bg.addColorStop(0, C.bgInner);
  bg.addColorStop(1, C.bgOuter);
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);

  // Bearing spokes every 30°.
  ctx.strokeStyle = C.spoke;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let deg = 0; deg < 360; deg += 30) {
    const a = (deg * Math.PI) / 180;
    ctx.moveTo(ox, oy);
    ctx.lineTo(ox + Math.sin(a) * reach, oy - Math.cos(a) * reach);
  }
  ctx.stroke();

  // Range rings: major rings every `step`, minor halfway between.
  const step = NICE_STEPS.find((s) => s * v.scale >= 110) ?? NICE_STEPS[NICE_STEPS.length - 1]!;
  const reachM = reach / v.scale;
  for (let r = step / 2; r <= reachM; r += step / 2) {
    const major = Math.round(r / step) * step === r;
    ctx.strokeStyle = major ? C.ringMajor : C.ringMinor;
    ctx.beginPath();
    ctx.arc(ox, oy, r * v.scale, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.font = `500 10px ${MONO}`;
  ctx.fillStyle = C.ringLabel;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  const labelAngle = (52 * Math.PI) / 180; // between the 030 and 060 spokes
  for (let r = step; r <= reachM; r += step) {
    const px = ox + Math.sin(labelAngle) * r * v.scale;
    const py = oy - Math.cos(labelAngle) * r * v.scale;
    ctx.fillText(r >= 1000 ? `${r / 1000} km` : `${r} m`, px + 4, py - 6);
  }

  // Compass bezel: on the 25 km ring normally, but never smaller than a readable size on
  // screen, so zooming far out doesn't crush its labels into the sensor.
  const br = Math.max(BEZEL_RANGE_M * v.scale, MIN_BEZEL_PX);
  ctx.strokeStyle = C.bezel;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.arc(ox, oy, br, 0, Math.PI * 2);
  ctx.stroke();
  ctx.textAlign = "center";
  ctx.font = `500 10px ${MONO}`;
  ctx.beginPath();
  for (let deg = 0; deg < 360; deg += 2) {
    const a = (deg * Math.PI) / 180;
    const len = deg % 30 === 0 ? 11 : deg % 10 === 0 ? 7 : 3.5;
    const dx = Math.sin(a);
    const dy = -Math.cos(a);
    ctx.moveTo(ox + dx * br, oy + dy * br);
    ctx.lineTo(ox + dx * (br - len), oy + dy * (br - len));
  }
  ctx.stroke();
  ctx.fillStyle = C.bezelLabel;
  for (let deg = 0; deg < 360; deg += 30) {
    const a = (deg * Math.PI) / 180;
    ctx.fillText(deg === 0 ? "N" : pad3(deg), ox + Math.sin(a) * (br + 15), oy - Math.cos(a) * (br + 15));
  }

  // Sweep: decorative, one clockwise revolution per scan interval. The conic gradient
  // runs clockwise from the sweep line, so the fading trail sits at the end of it.
  const period = Math.max(feed.stats.scanIntervalS, 0.25) * 1000;
  const sweepDeg = ((now % period) / period) * 360;
  if (layers.sweep && feed.frame) {
    const a = (sweepDeg * Math.PI) / 180;
    const grad = ctx.createConicGradient(a - Math.PI / 2, ox, oy);
    grad.addColorStop(0, `rgba(${C.sweep}, 0.15)`);
    grad.addColorStop(0.0001, `rgba(${C.sweep}, 0)`);
    grad.addColorStop(0.85, `rgba(${C.sweep}, 0)`);
    grad.addColorStop(0.97, `rgba(${C.sweep}, 0.05)`);
    grad.addColorStop(1, `rgba(${C.sweep}, 0.15)`);
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(ox, oy, reach, 0, Math.PI * 2);
    ctx.fill();

    const edge = ctx.createLinearGradient(ox, oy, ox + Math.sin(a) * reach, oy - Math.cos(a) * reach);
    edge.addColorStop(0, `rgba(${C.sweep}, 0.65)`);
    edge.addColorStop(1, `rgba(${C.sweep}, 0.05)`);
    ctx.strokeStyle = edge;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(ox, oy);
    ctx.lineTo(ox + Math.sin(a) * reach, oy - Math.cos(a) * reach);
    ctx.stroke();
  }

  // Sensor: the Vigil mark, standing on the origin.
  const halo = ctx.createRadialGradient(ox, oy, 0, ox, oy, 22);
  halo.addColorStop(0, "rgba(169, 200, 210, 0.22)");
  halo.addColorStop(1, "rgba(169, 200, 210, 0)");
  ctx.fillStyle = halo;
  ctx.fillRect(ox - 22, oy - 22, 44, 44);
  drawMark(ctx, ox, oy, 18, C.sensor);

  // Detections. With the sweep on, the newest scan glows like phosphor: brightest just
  // after the sweep passes its bearing, fading over the rest of the revolution.
  if (layers.detections) {
    const scans = feed.detectionHistory;
    scans.forEach((dets, idx) => {
      const age = scans.length - 1 - idx;
      for (const d of dets) {
        let alpha = 0.75 * Math.pow(0.5, age);
        if (layers.sweep && age === 0) {
          const since = (sweepDeg - bearingDeg(d.x, d.y) + 360) % 360;
          alpha = 0.3 + 0.7 * Math.pow(1 - since / 360, 1.6);
        }
        const px = sx(d.x);
        const py = sy(d.y);
        ctx.fillStyle = `rgba(${C.detection}, ${alpha * 0.16})`;
        ctx.beginPath();
        ctx.arc(px, py, 4.5, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = `rgba(${C.detection}, ${alpha})`;
        ctx.beginPath();
        ctx.arc(px, py, 1.6, 0, Math.PI * 2);
        ctx.fill();
      }
    });
  }

  const frame = feed.frame;
  if (!frame) return step;

  // Dead-reckon firm tracks forward between scans so motion is smooth.
  const dt = Math.min((now - feed.receivedAt) / 1000, 2 * feed.stats.scanIntervalS);
  const visible = frame.tracks.filter((t) => layers.tentative || t.status !== "tentative");
  const position = (t: TrackState) =>
    t.status === "tentative" ? { x: t.x, y: t.y } : { x: t.x + t.vx * dt, y: t.y + t.vy * dt };
  const focus = selectedId !== undefined && visible.some((t) => t.id === selectedId);
  const emphasis = (t: TrackState) => (!focus || t.id === selectedId || t.id === hoverId ? 1 : 0.4);

  // Tentative tracks first, so firm tracks draw on top of them.
  const ordered = [...visible].sort(
    (a, b) => Number(a.status !== "tentative") - Number(b.status !== "tentative"),
  );

  for (const t of ordered) {
    const p = position(t);
    const px = sx(p.x);
    const py = sy(p.y);
    const color = statusColor(t);
    const firm = t.status !== "tentative";
    ctx.globalAlpha = emphasis(t);

    if (layers.uncertainty && firm) {
      const r = 2 * t.posSigma * v.scale;
      if (r > 3) {
        const g = ctx.createRadialGradient(px, py, 0, px, py, r);
        g.addColorStop(0, hexAlpha(color, 0.16));
        g.addColorStop(1, hexAlpha(color, 0));
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(px, py, r, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // History dots, older ones smaller and fainter (air-traffic style).
    if (layers.trails && firm) {
      const trail = feed.trails.get(t.id) ?? [];
      const dots = trail.slice(-HISTORY_DOTS - 1, -1);
      dots.forEach((q, i) => {
        const k = (i + 1) / dots.length;
        ctx.fillStyle = hexAlpha(color, 0.12 + 0.5 * k);
        ctx.beginPath();
        ctx.arc(sx(q.x), sy(q.y), 0.9 + 1.1 * k, 0, Math.PI * 2);
        ctx.fill();
      });
    }

    if (!firm) {
      ctx.strokeStyle = hexAlpha(color, 0.75);
      ctx.lineWidth = 1;
      ctx.strokeRect(px - 3.5, py - 3.5, 7, 7);
      ctx.globalAlpha = 1;
      continue;
    }

    if (layers.leaders) {
      const ex = sx(p.x + t.vx * LEADER_SECONDS);
      const ey = sy(p.y + t.vy * LEADER_SECONDS);
      const g = ctx.createLinearGradient(px, py, ex, ey);
      g.addColorStop(0, hexAlpha(color, 0.85));
      g.addColorStop(1, hexAlpha(color, 0));
      ctx.strokeStyle = g;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(px, py);
      ctx.lineTo(ex, ey);
      ctx.stroke();
    }

    // Symbol, with a soft glow.
    ctx.shadowColor = color;
    ctx.shadowBlur = t.id === hoverId || t.id === selectedId ? 16 : 10;
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.8;
    ctx.setLineDash(t.status === "coasting" ? [3, 2.5] : []);
    ctx.beginPath();
    ctx.arc(px, py, 6, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);
    if (t.status === "confirmed") {
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(px, py, 1.6, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.shadowBlur = 0;

    if (t.id === selectedId) {
      const pulse = reducedMotion() ? 0 : Math.sin(now / 260) * 1.5;
      ctx.strokeStyle = hexAlpha(C.selected, 0.9);
      ctx.lineWidth = 1.3;
      const s = 13 + pulse;
      const k = 5;
      for (const [dx, dy] of [[-1, -1], [1, -1], [1, 1], [-1, 1]] as const) {
        ctx.beginPath();
        ctx.moveTo(px + dx * s, py + dy * (s - k));
        ctx.lineTo(px + dx * s, py + dy * s);
        ctx.lineTo(px + dx * (s - k), py + dy * s);
        ctx.stroke();
      }
    } else if (t.id === hoverId) {
      ctx.strokeStyle = hexAlpha(C.selected, 0.45);
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.arc(px, py, 11, 0, Math.PI * 2);
      ctx.stroke();
    }

    if (layers.dataBlocks || t.id === selectedId || t.id === hoverId) {
      // Blocks sit up-right of the symbol unless a panel covers that side.
      const rightEdge = w - insets.right - 8;
      drawDataBlock(ctx, t, feed.callsigns.get(t.id), px, py, color, t.id === selectedId, rightEdge);
    }
    ctx.globalAlpha = 1;
  }

  return step;
}

/**
 * Air-traffic-style label tied to the symbol: callsign on top, then speed and heading
 * with the tracker's own track number dimmed at the end.
 */
function drawDataBlock(
  ctx: CanvasRenderingContext2D,
  t: TrackState,
  callsign: string | undefined,
  px: number,
  py: number,
  color: string,
  selected: boolean,
  rightEdge: number,
) {
  const line1 = callsign ?? `T${t.id}`;
  const line2 = `${speed(t.vx, t.vy).toFixed(0)} ${pad3(headingDeg(t.vx, t.vy))}°`;
  const idText = callsign ? `T${t.id}` : "";
  ctx.font = `600 11px ${MONO}`;
  const w1 = ctx.measureText(line1).width;
  ctx.font = `400 10px ${MONO}`;
  const w2 = ctx.measureText(line2).width;
  const wId = idText ? ctx.measureText(idText).width + 10 : 0;
  const bw = Math.max(w1, w2 + wId) + 12;
  const bh = 30;
  const flip = px + 16 + bw > rightEdge; // mirror to the left if it would run under a panel
  const bx = flip ? px - 16 - bw : px + 16;
  const by = py - 16 - bh;

  ctx.strokeStyle = hexAlpha(color, 0.4);
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(flip ? px - 5 : px + 5, py - 5);
  ctx.lineTo(flip ? bx + bw : bx, by + bh);
  ctx.stroke();

  ctx.fillStyle = C.blockBg;
  ctx.beginPath();
  ctx.roundRect(bx, by, bw, bh, 4);
  ctx.fill();
  ctx.strokeStyle = hexAlpha(selected ? C.selected : color, selected ? 0.7 : 0.35);
  ctx.stroke();

  ctx.textAlign = "left";
  ctx.textBaseline = "alphabetic";
  ctx.fillStyle = selected ? C.selected : color;
  ctx.font = `600 11px ${MONO}`;
  ctx.fillText(line1, bx + 6, by + 13);
  ctx.fillStyle = C.blockText;
  ctx.font = `400 10px ${MONO}`;
  ctx.fillText(line2, bx + 6, by + 25);
  if (idText) {
    ctx.fillStyle = C.blockId;
    ctx.textAlign = "right";
    ctx.fillText(idText, bx + bw - 6, by + 25);
  }
}

// Canvas versions of the logo paths, built once.
const MARK = {
  pillar: new Path2D(PILLAR),
  chevron: new Path2D(CHEVRON),
  // Everything except the cut region, as an even-odd clip.
  keep: (() => {
    const p = new Path2D();
    p.rect(0, 0, 1254, 1254);
    p.addPath(new Path2D(PILLAR_CUT));
    return p;
  })(),
};

/** The Vigil mark, `size` px tall, centred on (x, y). */
function drawMark(ctx: CanvasRenderingContext2D, x: number, y: number, size: number, color: string) {
  const k = size / 1040; // the artwork spans ~1040 units
  ctx.save();
  ctx.translate(x - 620 * k, y - 624 * k); // artwork centre is (620, 624)
  ctx.scale(k, k);
  ctx.fillStyle = color;
  ctx.fill(MARK.chevron);
  ctx.clip(MARK.keep, "evenodd");
  ctx.fill(MARK.pillar);
  ctx.restore();
}

/** "#rrggbb" plus alpha as an rgba() string. */
function hexAlpha(hex: string, alpha: number): string {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, ${alpha})`;
}
