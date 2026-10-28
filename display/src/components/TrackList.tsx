import { headingDeg, pad3, range, speed } from "../geometry";
import type { TrackState } from "../shared/protocol";
import { Glyph } from "./Glyph";

interface TrackListProps {
  tracks: TrackState[];
  callsigns: ReadonlyMap<number, string>;
  selectedId: number | undefined;
  onSelect: (id: number | undefined) => void;
}

export function TrackList({ tracks, callsigns, selectedId, onSelect }: TrackListProps) {
  const rows = [...tracks].sort((a, b) => a.id - b.id);
  if (rows.length === 0) return <p className="empty">No tracks yet.</p>;

  return (
    <table className="track-list">
      <thead>
        <tr>
          <th scope="col">Track</th>
          <th scope="col" className="num">Range</th>
          <th scope="col" className="num">Speed</th>
          <th scope="col" className="num">Hdg</th>
        </tr>
      </thead>
      <tbody>
        {rows.map((t) => {
          const hdg = headingDeg(t.vx, t.vy);
          const name = callsigns.get(t.id);
          return (
            <tr
              key={t.id}
              className={`row-${t.status}${t.id === selectedId ? " selected" : ""}`}
              onClick={() => onSelect(t.id === selectedId ? undefined : t.id)}
              aria-selected={t.id === selectedId}
            >
              <td className="track-cell">
                <span className="track-id">
                  <Glyph kind={t.status} size={12} />
                  <span className="track-names">
                    <span className="track-name" title={name}>
                      {name ?? `T${t.id}`}
                    </span>
                    {name && <span className="track-number">T{t.id}</span>}
                  </span>
                </span>
              </td>
              <td className="num">
                {(range(t.x, t.y) / 1000).toFixed(1)}
                <small>km</small>
              </td>
              <td className="num">
                {speed(t.vx, t.vy).toFixed(0)}
                <small>m/s</small>
              </td>
              <td className="num">
                <span className="hdg">
                  <svg viewBox="0 0 10 10" style={{ transform: `rotate(${hdg}deg)` }} aria-hidden="true">
                    <path d="M5 0.5 L8.5 9 L5 7 L1.5 9 Z" />
                  </svg>
                  {pad3(hdg)}°
                </span>
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
