import { useEffect, useMemo, useRef, useState } from "react";

import { EventLog } from "./components/EventLog";
import { Glyph } from "./components/Glyph";
import { HeadingDial } from "./components/HeadingDial";
import { Logo } from "./components/Logo";
import { Sparkline } from "./components/Sparkline";
import { TrackList } from "./components/TrackList";
import { MPS_TO_KNOTS, clock, quantile } from "./format";
import { bearingDeg, headingDeg, pad3, range, speed } from "./geometry";
import { Scope, type Insets, type Layers } from "./Scope";
import type { TrackState, TrackStatus } from "./shared/protocol";
import { trackLabel, useTrackFeed, type Feed } from "./useTrackFeed";

type FeedHealth = "live" | "stale" | "no-feed" | "connecting" | "disconnected";

const HEALTH_LABEL: Record<FeedHealth, string> = {
  live: "Live",
  stale: "Stale",
  "no-feed": "No feed",
  connecting: "Connecting",
  disconnected: "Gateway down",
};

const LAYER_LABELS: Record<keyof Layers, string> = {
  detections: "Detections",
  trails: "History",
  leaders: "Leaders",
  uncertainty: "Uncertainty",
  dataBlocks: "Data blocks",
  tentative: "Tentative",
  sweep: "Sweep",
};

const STATUS_LABEL: Record<TrackStatus, string> = {
  confirmed: "Confirmed",
  coasting: "Coasting",
  tentative: "Tentative",
};

const STATUS_COLOR: Record<TrackStatus, string> = {
  confirmed: "var(--confirmed)",
  coasting: "var(--coasting)",
  tentative: "var(--tentative)",
};

const LATENCY_WINDOW = 120;
const PANEL_WIDTH = 380;
const DISPLAY_CARD_WIDTH = 230;
const GAP = 14;
const HEADER_HEIGHT = 56;

function health(feed: Feed, now: number): FeedHealth {
  if (feed.link === "connecting") return "connecting";
  if (feed.link === "closed") return "disconnected";
  if (!feed.frame) return "no-feed";
  const staleAfterMs = Math.max(3 * feed.stats.scanIntervalS * 1000, 3000);
  return now - feed.receivedAt > staleAfterMs ? "stale" : "live";
}

function useMediaQuery(query: string): boolean {
  const [matches, setMatches] = useState(() => window.matchMedia(query).matches);
  useEffect(() => {
    const mql = window.matchMedia(query);
    const onChange = () => setMatches(mql.matches);
    mql.addEventListener("change", onChange);
    return () => mql.removeEventListener("change", onChange);
  }, [query]);
  return matches;
}

export function App() {
  const feed = useTrackFeed();
  const compact = useMediaQuery("(max-width: 900px)");
  const [layers, setLayers] = useState<Layers>(() => ({
    detections: true,
    trails: true,
    leaders: true,
    uncertainty: true,
    dataBlocks: true,
    tentative: false,
    sweep: !window.matchMedia("(prefers-reduced-motion: reduce)").matches,
  }));
  const [selectedId, setSelectedId] = useState<number>();
  const [tab, setTab] = useState<"tracks" | "events">("tracks");
  // On narrower desktops the display card starts collapsed to leave room for the scope.
  const [displayOpen, setDisplayOpen] = useState(() => window.innerWidth >= 1400);

  // Re-evaluate feed health and the UTC clock even when no frames arrive.
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(performance.now()), 500);
    return () => window.clearInterval(timer);
  }, []);

  // Escape clears the selection.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && setSelectedId(undefined);
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  // Screen space covered by floating panels. The scope centres the sensor, and fits
  // everything, within what's left.
  const insets: Insets = useMemo(
    () =>
      compact
        ? { top: 0, right: 0, bottom: 0, left: 0 }
        : {
            top: HEADER_HEIGHT + 2 * GAP,
            right: PANEL_WIDTH + 2 * GAP,
            bottom: 0,
            left: displayOpen ? DISPLAY_CARD_WIDTH + 2 * GAP : 0,
          },
    [compact, displayOpen],
  );

  const frame = feed.frame;
  const tracks = frame?.tracks ?? [];
  const count = (s: TrackStatus) => tracks.filter((t) => t.status === s).length;
  const listed = layers.tentative ? tracks : tracks.filter((t) => t.status !== "tentative");
  const selected = tracks.find((t) => t.id === selectedId);
  // Remember the selected track's name: once it drops, its callsign is freed for reuse.
  const selectedName = useRef<string>(undefined);
  if (selected) selectedName.current = trackLabel(feed.callsigns, selected.id);
  const state = health(feed, now);

  const latency = feed.stats.latencyUs;
  const fmtUs = (us: number | undefined) => (us === undefined ? "—" : us.toFixed(1));
  const mean = latency.length ? latency.reduce((a, b) => a + b, 0) / latency.length : undefined;

  const select = (id: number | undefined) => setSelectedId(id);

  return (
    <div className={compact ? "app compact" : "app"}>
      <header className="topbar glass">
        <div className="brand">
          <Logo size={30} className="brand-mark" title="Vigil" />
        </div>
        <div className="topbar-right">
          <span className="readout">
            <span className="readout-key">SCAN</span>
            <span className="readout-val">{frame ? frame.seq.toLocaleString() : "—"}</span>
          </span>
          <span className="readout">
            <span className="readout-key">T+</span>
            <span className="readout-val">{frame ? clock(frame.timeS) : "--:--:--"}</span>
          </span>
          <span className="readout hide-small">
            <span className="readout-key">UTC</span>
            <span className="readout-val">{new Date().toISOString().slice(11, 19)}</span>
          </span>
          <span className={`health health-${state}`} role="status">
            <span className="health-dot" aria-hidden="true" />
            {HEALTH_LABEL[state]}
          </span>
        </div>
      </header>

      <main className="stage">
        <Scope feed={feed} layers={layers} insets={insets} selectedId={selectedId} onSelect={select} />

        <section
          className={compact || displayOpen ? "display-card glass" : "display-card glass collapsed"}
          aria-label="Display options"
        >
          {!compact && (
            <button
              type="button"
              className="display-toggle"
              aria-expanded={displayOpen}
              onClick={() => setDisplayOpen((o) => !o)}
            >
              <svg viewBox="0 0 16 16" aria-hidden="true" className="layers-icon">
                <path d="M8 2 14.5 5.5 8 9 1.5 5.5Z" />
                <path d="M1.5 8.5 8 12l6.5-3.5" />
              </svg>
              Display
              <svg viewBox="0 0 16 16" aria-hidden="true" className="chevron">
                <path d="m4 6 4 4 4-4" />
              </svg>
            </button>
          )}
          {(compact || displayOpen) && (
            <div className="display-body">
          <div className="legend">
            {(["confirmed", "coasting", "tentative"] as const).map((s) => (
              <span key={s} className="legend-item" style={{ color: STATUS_COLOR[s] }}>
                <Glyph kind={s} size={13} />
                <span>{STATUS_LABEL[s]}</span>
              </span>
            ))}
            <span className="legend-item legend-detection">
              <Glyph kind="detection" size={13} />
              <span>Detection</span>
            </span>
          </div>
          <div className="switches">
            {(Object.keys(LAYER_LABELS) as (keyof Layers)[]).map((key) => (
              <label key={key} className="switch">
                <input
                  type="checkbox"
                  role="switch"
                  checked={layers[key]}
                  onChange={() => setLayers((l) => ({ ...l, [key]: !l[key] }))}
                />
                <span className="switch-track" aria-hidden="true" />
                {LAYER_LABELS[key]}
              </label>
            ))}
          </div>
            </div>
          )}
        </section>

        {(state === "no-feed" || state === "connecting" || state === "disconnected") && (
          <div className="scope-message" style={{ paddingRight: insets.right, paddingTop: insets.top }}>
            <div className="glass">
              <Logo size={56} className={`message-mark${state === "connecting" ? " breathing" : ""}`} />
              <h2>{HEALTH_LABEL[state]}</h2>
              <p>
                {state === "no-feed"
                  ? "Waiting for the tracker. Start it with ./demo.sh, or run vigil-tracker and vigil-sim."
                  : state === "connecting"
                    ? "Connecting to the display gateway…"
                    : "Can't reach the display gateway. Is npm run dev (or ./demo.sh) running?"}
              </p>
            </div>
          </div>
        )}

        <aside className="panel" aria-label="Track picture">
          <section className="section glass">
            <h2 className="section-title">Picture</h2>
            <div className="status-tiles">
              {(["confirmed", "coasting", "tentative"] as const).map((s) => (
                <div key={s} className={`tile tile-${s}`}>
                  <div className="tile-label">
                    <Glyph kind={s} size={12} />
                    {STATUS_LABEL[s]}
                  </div>
                  <div className="tile-value">{count(s)}</div>
                </div>
              ))}
            </div>
            <dl className="inline-stats">
              <div>
                <dt>Detections</dt>
                <dd>{frame?.detections.length ?? "—"}</dd>
              </div>
              <div>
                <dt>Scan rate</dt>
                <dd>{frame ? `${(1 / feed.stats.scanIntervalS).toFixed(1)} Hz` : "—"}</dd>
              </div>
            </dl>
          </section>

          <section className="section glass">
            <h2 className="section-title">
              Tracker latency <span className="unit">µs per scan</span>
            </h2>
            <div className="hero">
              <span className="hero-value">{fmtUs(latency.at(-1))}</span>
              <span className="hero-unit">µs</span>
            </div>
            <Sparkline
              values={latency}
              capacity={LATENCY_WINDOW}
              format={(v) => `${v.toFixed(1)} µs`}
              label="Tracker latency"
            />
            <dl className="inline-stats three">
              <div>
                <dt>Mean</dt>
                <dd>{fmtUs(mean)}</dd>
              </div>
              <div>
                <dt>p95</dt>
                <dd>{fmtUs(quantile(latency, 0.95))}</dd>
              </div>
              <div>
                <dt>Max</dt>
                <dd>{fmtUs(latency.length ? Math.max(...latency) : undefined)}</dd>
              </div>
            </dl>
          </section>

          <section className="section glass" aria-live="polite">
            <h2 className="section-title">Selected track</h2>
            {selected ? (
              <TrackDetail
                track={selected}
                name={feed.callsigns.get(selected.id)}
                onClose={() => select(undefined)}
              />
            ) : (
              <p className="empty">
                {selectedId !== undefined
                  ? `${selectedName.current ?? `T${selectedId}`} has been dropped.`
                  : "Click a track on the scope or in the list."}
              </p>
            )}
          </section>

          <section className="section glass grow">
            <div className="tabs" role="tablist">
              <button
                type="button"
                role="tab"
                aria-selected={tab === "tracks"}
                onClick={() => setTab("tracks")}
              >
                Tracks <span className="count">{listed.length}</span>
              </button>
              <button
                type="button"
                role="tab"
                aria-selected={tab === "events"}
                onClick={() => setTab("events")}
              >
                Events <span className="count">{feed.events.length}</span>
              </button>
            </div>
            {tab === "tracks" ? (
              <TrackList
                tracks={listed}
                callsigns={feed.callsigns}
                selectedId={selectedId}
                onSelect={select}
              />
            ) : (
              <EventLog events={feed.events} onSelect={select} />
            )}
          </section>
        </aside>
      </main>
    </div>
  );
}

interface TrackDetailProps {
  track: TrackState;
  name: string | undefined;
  onClose: () => void;
}

function TrackDetail({ track: t, name, onClose }: TrackDetailProps) {
  const spd = speed(t.vx, t.vy);
  const hdg = headingDeg(t.vx, t.vy);
  const color = STATUS_COLOR[t.status];
  // Position uncertainty against a 100 m scale: short bar = well-tracked.
  const sigmaPct = Math.min(100, (t.posSigma / 100) * 100);

  return (
    <div className="detail">
      <HeadingDial headingDeg={hdg} color={color} />
      <div className="detail-body">
        <div className="detail-head">
          <span className="detail-name" title={name}>
            {name ?? `T${t.id}`}
          </span>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Clear selection">
            <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 4l8 8M12 4l-8 8" /></svg>
          </button>
        </div>
        <div className="detail-sub">
          <span className={`pill pill-${t.status}`}>{STATUS_LABEL[t.status]}</span>
          {name && <span className="detail-number">Track {t.id}</span>}
        </div>
        <dl className="detail-grid">
          <div>
            <dt>Range</dt>
            <dd>{(range(t.x, t.y) / 1000).toFixed(2)} km</dd>
          </div>
          <div>
            <dt>Bearing</dt>
            <dd>{pad3(bearingDeg(t.x, t.y))}°</dd>
          </div>
          <div>
            <dt>Speed</dt>
            <dd>
              {spd.toFixed(0)} m/s <small>{(spd * MPS_TO_KNOTS).toFixed(0)} kt</small>
            </dd>
          </div>
          <div>
            <dt>Heading</dt>
            <dd>{pad3(hdg)}°</dd>
          </div>
          <div>
            <dt>Hits</dt>
            <dd>{t.hits}</dd>
          </div>
          <div>
            <dt>Vel. σ</dt>
            <dd>{t.velSigma.toFixed(1)} m/s</dd>
          </div>
        </dl>
        <div className="sigma">
          <div className="sigma-label">
            <span>Position σ</span>
            <span>{t.posSigma.toFixed(1)} m</span>
          </div>
          <div className="sigma-bar" role="meter" aria-valuemin={0} aria-valuemax={100} aria-valuenow={t.posSigma}>
            <span style={{ width: `${sigmaPct}%`, background: color }} />
          </div>
        </div>
      </div>
    </div>
  );
}
