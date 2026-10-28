import { useId } from "react";

// The Vigil mark: a domed sentinel pillar crossed by a chevron, with gaps cut through the
// pillar on both sides of the chevron. Coordinates are in the original artwork's 1254 px
// space (docs/brand/vigil-logo.png), fitted from its edges: it matches the PNG to 99.3%
// pixel overlap. The artwork is hand-tuned, so the sides are not quite symmetric.

/** Pillar with its rounded top. */
export const PILLAR = "M516.8 1126.1V231.1a110.2 110.2 0 0 1 220.4 0v895z";
/** The chevron band. */
export const CHEVRON = "M114.7 833 630 409.6 1126.4 833H908.7L629.3 594 336.3 833z";
/** Region removed from the pillar: the chevron plus a gap above and below it. */
export const PILLAR_CUT = "M0 923.6 626.5 349.6 1254 924.9v236.4L626.2 642 0 1160.2z";
/** Square box centred on the artwork. */
export const VIEWBOX = "100 104 1040 1040";

interface LogoProps {
  size?: number;
  /** Accessible name; omit when a visible wordmark sits beside the mark. */
  title?: string;
  className?: string;
}

export function Logo({ size = 28, title, className }: LogoProps) {
  const maskId = useId();
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox={VIEWBOX}
      role={title ? "img" : undefined}
      aria-hidden={title ? undefined : true}
      aria-label={title}
    >
      <mask id={maskId}>
        <rect x="0" y="0" width="1254" height="1254" fill="#fff" />
        <path d={PILLAR_CUT} fill="#000" />
      </mask>
      <path d={PILLAR} fill="currentColor" mask={`url(#${maskId})`} />
      <path d={CHEVRON} fill="currentColor" />
    </svg>
  );
}
