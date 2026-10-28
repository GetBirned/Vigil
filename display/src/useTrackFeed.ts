import { useEffect, useRef, useState } from "react";

import { CallsignAllocator } from "./shared/callsigns";
import type { Detection, Frame, TrackStatus } from "./shared/protocol";

export type LinkState = "connecting" | "open" | "closed";

export interface Point {
  x: number;
  y: number;
}

export interface FeedStats {
  /** Tracker processing time for recent scans, microseconds (newest last). */
  latencyUs: number[];
  /** Seconds between scans, estimated from scan timestamps. */
  scanIntervalS: number;
}

export type TrackEventKind = "confirmed" | "coasting" | "reacquired" | "dropped";

export interface TrackEvent {
  key: number;
  timeS: number;
  trackId: number;
  /** Callsign at the time of the event (kept, since a dropped track's name is reused). */
  name: string | undefined;
  kind: TrackEventKind;
}

export interface Feed {
  link: LinkState;
  frame: Frame | undefined;
  /** performance.now() when `frame` arrived; used to dead-reckon between scans. */
  receivedAt: number;
  /** Recent positions per track id, oldest first. */
  trails: Map<number, Point[]>;
  /** Detections from recent scans, newest last. */
  detectionHistory: Detection[][];
  /** Lifecycle changes of firm tracks, newest first. */
  events: TrackEvent[];
  /** Display callsign per firm track id. Tentative tracks have none. */
  callsigns: ReadonlyMap<number, string>;
  stats: FeedStats;
}

/** A track's display name: its callsign once it has one, otherwise its number. */
export const trackLabel = (callsigns: ReadonlyMap<number, string>, id: number): string =>
  callsigns.get(id) ?? `T${id}`;

const TRAIL_LENGTH = 40;
const DETECTION_SCANS = 6;
const LATENCY_SAMPLES = 120;
const EVENT_LIMIT = 60;

function socketUrl(): string {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  return `${scheme}://${location.host}/ws`;
}

/** Subscribes to the gateway's WebSocket and keeps a short history for drawing. */
export function useTrackFeed(): Feed {
  const [link, setLink] = useState<LinkState>("connecting");
  const [frame, setFrame] = useState<Frame>();

  // History lives in refs: it changes every scan and is only read while drawing.
  const receivedAt = useRef(0);
  const trails = useRef(new Map<number, Point[]>());
  const detectionHistory = useRef<Detection[][]>([]);
  const events = useRef<TrackEvent[]>([]);
  const lastStatus = useRef(new Map<number, TrackStatus>());
  const eventKey = useRef(0);
  const callsigns = useRef(new CallsignAllocator());
  const stats = useRef<FeedStats>({ latencyUs: [], scanIntervalS: 1 });
  const lastTime = useRef<number | undefined>(undefined);

  useEffect(() => {
    let socket: WebSocket | undefined;
    let retryTimer: number | undefined;
    let retryMs = 500;
    let disposed = false;

    const pushEvent = (timeS: number, trackId: number, kind: TrackEventKind) => {
      const name = callsigns.current.get(trackId);
      events.current.unshift({ key: eventKey.current++, timeS, trackId, name, kind });
      if (events.current.length > EVENT_LIMIT) events.current.pop();
    };

    const onFrame = (f: Frame) => {
      // The first frame we see (on page load or after a restart) is a baseline, not news.
      const baseline = lastTime.current === undefined || f.timeS < lastTime.current;

      // Time going backwards means the simulator restarted: forget the old picture.
      if (lastTime.current !== undefined && f.timeS < lastTime.current) {
        trails.current.clear();
        detectionHistory.current = [];
        events.current = [];
        lastStatus.current.clear();
        callsigns.current.clear();
        stats.current.latencyUs = [];
      } else if (lastTime.current !== undefined && f.timeS > lastTime.current) {
        const dt = f.timeS - lastTime.current;
        stats.current.scanIntervalS = 0.8 * stats.current.scanIntervalS + 0.2 * dt;
      }
      lastTime.current = f.timeS;

      // Every firm track gets a callsign. Lowest track number first, so the oldest tracks
      // get the earliest names in the pool.
      for (const t of [...f.tracks].sort((a, b) => a.id - b.id)) {
        if (t.status !== "tentative") callsigns.current.assign(t.id);
      }

      // Lifecycle events. Tentative tracks come and go every scan, so only changes that
      // involve a firm (confirmed or coasting) track are worth an operator's attention.
      const seen = new Map<number, TrackStatus>();
      for (const t of f.tracks) {
        seen.set(t.id, t.status);
        const before = lastStatus.current.get(t.id);
        if (baseline || t.status === before) continue;
        if (t.status === "confirmed") {
          pushEvent(f.timeS, t.id, before === "coasting" ? "reacquired" : "confirmed");
        } else if (t.status === "coasting") {
          pushEvent(f.timeS, t.id, "coasting");
        }
      }
      for (const [id, status] of lastStatus.current) {
        if (seen.has(id)) continue;
        if (!baseline && status !== "tentative") pushEvent(f.timeS, id, "dropped");
        callsigns.current.release(id); // after the event, so it records the name
      }
      lastStatus.current = seen;

      const alive = new Set<number>();
      for (const t of f.tracks) {
        alive.add(t.id);
        const trail = trails.current.get(t.id) ?? [];
        trail.push({ x: t.x, y: t.y });
        if (trail.length > TRAIL_LENGTH) trail.shift();
        trails.current.set(t.id, trail);
      }
      for (const id of trails.current.keys()) {
        if (!alive.has(id)) trails.current.delete(id);
      }

      detectionHistory.current.push(f.detections);
      if (detectionHistory.current.length > DETECTION_SCANS) detectionHistory.current.shift();

      stats.current.latencyUs.push(f.processingUs);
      if (stats.current.latencyUs.length > LATENCY_SAMPLES) stats.current.latencyUs.shift();

      receivedAt.current = performance.now();
      setFrame(f);
    };

    const connect = () => {
      setLink("connecting");
      socket = new WebSocket(socketUrl());
      socket.onopen = () => {
        retryMs = 500;
        setLink("open");
      };
      socket.onmessage = (event) => {
        const msg = JSON.parse(event.data as string) as Frame;
        if (msg.type === "frame") onFrame(msg);
      };
      socket.onclose = () => {
        if (disposed) return;
        setLink("closed");
        retryTimer = window.setTimeout(connect, retryMs);
        retryMs = Math.min(retryMs * 2, 5000);
      };
    };

    connect();
    return () => {
      disposed = true;
      window.clearTimeout(retryTimer);
      socket?.close();
    };
  }, []);

  return {
    link,
    frame,
    receivedAt: receivedAt.current,
    trails: trails.current,
    detectionHistory: detectionHistory.current,
    events: events.current,
    callsigns: callsigns.current.names,
    stats: stats.current,
  };
}
