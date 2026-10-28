import type { TrackStatus } from "../shared/protocol";

/** The same symbols the scope draws, for legends and lists. */
export function Glyph({ kind, size = 14 }: { kind: TrackStatus | "detection"; size?: number }) {
  const c = size / 2;
  return (
    <svg className={`glyph glyph-${kind}`} width={size} height={size} viewBox={`0 0 ${size} ${size}`} aria-hidden="true">
      {kind === "confirmed" && (
        <>
          <circle cx={c} cy={c} r={c - 2} fill="none" stroke="currentColor" strokeWidth="1.6" />
          <circle cx={c} cy={c} r="1.4" fill="currentColor" />
        </>
      )}
      {kind === "coasting" && (
        <circle cx={c} cy={c} r={c - 2} fill="none" stroke="currentColor" strokeWidth="1.6" strokeDasharray="2.4 2" />
      )}
      {kind === "tentative" && (
        <rect x={c - 3.5} y={c - 3.5} width="7" height="7" fill="none" stroke="currentColor" strokeWidth="1.3" />
      )}
      {kind === "detection" && <circle cx={c} cy={c} r="1.8" fill="currentColor" />}
    </svg>
  );
}
