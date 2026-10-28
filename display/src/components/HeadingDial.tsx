/** Small compass card with the track's heading as a needle. */
export function HeadingDial({ headingDeg, color }: { headingDeg: number; color: string }) {
  const ticks = Array.from({ length: 36 }, (_, i) => i * 10);
  return (
    <svg className="dial" viewBox="-50 -50 100 100" role="img" aria-label={`Heading ${Math.round(headingDeg)} degrees`}>
      <circle r="46" className="dial-face" />
      {ticks.map((deg) => {
        const major = deg % 90 === 0;
        const a = (deg * Math.PI) / 180;
        const r1 = 46;
        const r2 = major ? 38 : 42;
        return (
          <line
            key={deg}
            x1={Math.sin(a) * r1}
            y1={-Math.cos(a) * r1}
            x2={Math.sin(a) * r2}
            y2={-Math.cos(a) * r2}
            className={major ? "dial-tick major" : "dial-tick"}
          />
        );
      })}
      {(["N", "E", "S", "W"] as const).map((label, i) => {
        const a = (i * Math.PI) / 2;
        return (
          <text key={label} x={Math.sin(a) * 29} y={-Math.cos(a) * 29} className="dial-label">
            {label}
          </text>
        );
      })}
      <g style={{ transform: `rotate(${headingDeg}deg)`, transition: "transform 600ms ease-out" }}>
        <path d="M0,-40 L5,-6 L0,-10 L-5,-6 Z" fill={color} />
        <line x1="0" y1="-6" x2="0" y2="20" stroke={color} strokeOpacity="0.35" strokeWidth="2" />
      </g>
      <circle r="3" fill="var(--bg-1)" stroke={color} strokeWidth="1.5" />
    </svg>
  );
}
