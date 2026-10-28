import { useId, useState, type PointerEvent } from "react";

interface SparklineProps {
  values: number[];
  /** Total samples the window holds, so a filling buffer grows from the left. */
  capacity: number;
  format: (v: number) => string;
  label: string;
  height?: number;
}

/** Single-series trend line with a hover readout. Recessive: no axes, faint baseline. */
export function Sparkline({ values, capacity, format, label, height = 44 }: SparklineProps) {
  const gradientId = useId();
  const [hover, setHover] = useState<number>();
  const width = 300; // viewBox units; the SVG scales to its container

  if (values.length < 2) {
    return <div className="sparkline-empty" style={{ height }}>Collecting samples…</div>;
  }

  const max = Math.max(...values) * 1.15 || 1;
  const pad = 3;
  const step = width / (capacity - 1);
  const x0 = width - (values.length - 1) * step;
  const x = (i: number) => x0 + i * step;
  const y = (v: number) => height - pad - (v / max) * (height - 2 * pad);
  const line = values.map((v, i) => `${i ? "L" : "M"}${x(i).toFixed(1)},${y(v).toFixed(1)}`).join("");
  const area = `${line}L${x(values.length - 1).toFixed(1)},${height}L${x(0).toFixed(1)},${height}Z`;

  const onMove = (e: PointerEvent<SVGSVGElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const vx = ((e.clientX - rect.left) / rect.width) * width;
    const i = Math.round((vx - x0) / step);
    setHover(i >= 0 && i < values.length ? i : undefined);
  };

  const hv = hover !== undefined ? values[hover] : undefined;
  const last = values[values.length - 1]!;

  return (
    <div className="sparkline">
      <svg
        viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="none"
        style={{ height }}
        role="img"
        aria-label={`${label}: latest ${format(last)}`}
        onPointerMove={onMove}
        onPointerLeave={() => setHover(undefined)}
      >
        <defs>
          <linearGradient id={gradientId} x1="0" x2="0" y1="0" y2="1">
            <stop offset="0" stopColor="var(--accent)" stopOpacity="0.28" />
            <stop offset="1" stopColor="var(--accent)" stopOpacity="0" />
          </linearGradient>
        </defs>
        <line x1="0" x2={width} y1={height - 0.5} y2={height - 0.5} className="sparkline-base" />
        <path d={area} fill={`url(#${gradientId})`} />
        <path d={line} className="sparkline-line" vectorEffect="non-scaling-stroke" />
        {hv !== undefined && hover !== undefined && (
          <line x1={x(hover)} x2={x(hover)} y1="0" y2={height} className="sparkline-cursor" vectorEffect="non-scaling-stroke" />
        )}
      </svg>
      {hv !== undefined && hover !== undefined && (
        <div
          className="sparkline-tip"
          style={{ left: `${(x(hover) / width) * 100}%` }}
        >
          {format(hv)}
          <span>{values.length - 1 - hover === 0 ? "latest" : `${values.length - 1 - hover} scans ago`}</span>
        </div>
      )}
    </div>
  );
}
