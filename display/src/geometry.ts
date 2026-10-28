// Conversions between the tracker's east/north frame and operator units.

/** Distance from the sensor at the origin, in metres. */
export const range = (x: number, y: number): number => Math.hypot(x, y);

/** Compass bearing from the sensor, degrees: 0 = north, clockwise. */
export function bearingDeg(x: number, y: number): number {
  const deg = (Math.atan2(x, y) * 180) / Math.PI;
  return deg < 0 ? deg + 360 : deg;
}

/** Compass heading of a velocity vector, degrees: 0 = north, clockwise. */
export const headingDeg = bearingDeg;

export const speed = (vx: number, vy: number): number => Math.hypot(vx, vy);

export const pad3 = (deg: number): string => Math.round(deg % 360).toString().padStart(3, "0");
