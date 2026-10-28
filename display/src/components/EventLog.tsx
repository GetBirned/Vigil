import type { TrackEvent, TrackEventKind } from "../useTrackFeed";
import { clock } from "../format";

const TEXT: Record<TrackEventKind, string> = {
  confirmed: "confirmed",
  coasting: "lost contact, coasting",
  reacquired: "reacquired",
  dropped: "dropped",
};

interface EventLogProps {
  events: TrackEvent[];
  onSelect: (id: number) => void;
}

export function EventLog({ events, onSelect }: EventLogProps) {
  if (events.length === 0) return <p className="empty">Track events will appear here.</p>;
  return (
    <ol className="event-log">
      {events.map((e) => (
        <li key={e.key} className={`event event-${e.kind}`}>
          <span className="event-time">{clock(e.timeS)}</span>
          <span className="event-dot" aria-hidden="true" />
          <span className="event-text">
            {e.kind === "dropped" ? (
              <b>{e.name ?? `T${e.trackId}`}</b>
            ) : (
              <button type="button" className="link" onClick={() => onSelect(e.trackId)}>
                {e.name ?? `T${e.trackId}`}
              </button>
            )}{" "}
            {TEXT[e.kind]}
          </span>
        </li>
      ))}
    </ol>
  );
}
