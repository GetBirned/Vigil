// Decoder for the Vigil UDP messages (docs/ICD.md) and the JSON frames the gateway
// sends to browsers. Shared by the gateway (Node) and the web app.

export const MAGIC = 0x56474c31; // "VGL1"
export const VERSION = 1;
export const MSG_DETECTION_REPORT = 1;
export const MSG_TRACK_REPORT = 2;

export const HEADER_SIZE = 24;
export const DETECTION_SIZE = 16;
export const TRACK_SUMMARY_SIZE = 8;
export const TRACK_SIZE = 48;

export type TrackStatus = "tentative" | "confirmed" | "coasting";
const STATUS_BY_CODE: readonly TrackStatus[] = ["tentative", "confirmed", "coasting"];

export interface Detection {
  x: number; // metres east
  y: number; // metres north
}

export interface TrackState {
  id: number;
  status: TrackStatus;
  hits: number;
  x: number;
  y: number;
  vx: number;
  vy: number;
  posSigma: number;
  velSigma: number;
}

export interface DetectionReport {
  kind: "detections";
  seq: number;
  sensorId: number;
  timeS: number;
  detections: Detection[];
}

export interface TrackReport {
  kind: "tracks";
  seq: number;
  sourceId: number;
  timeS: number;
  processingUs: number;
  detectionsInScan: number;
  tracks: TrackState[];
}

export interface DecodeFailure {
  kind: "error";
  reason: string;
}

export type Decoded = DetectionReport | TrackReport | DecodeFailure;

/** One scan's worth of picture, as the gateway sends it to browsers (JSON). */
export interface Frame {
  type: "frame";
  seq: number;
  timeS: number;
  processingUs: number;
  detections: Detection[];
  tracks: TrackState[];
}

const fail = (reason: string): DecodeFailure => ({ kind: "error", reason });

export function decodeDatagram(bytes: Uint8Array): Decoded {
  if (bytes.byteLength < HEADER_SIZE) return fail("datagram shorter than header");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  if (view.getUint32(0, true) !== MAGIC) return fail("bad magic number");
  if (view.getUint16(4, true) !== VERSION) return fail("unsupported protocol version");
  const type = view.getUint16(6, true);
  const seq = view.getUint32(8, true);
  const sourceId = view.getUint16(12, true);
  const count = view.getUint16(14, true);
  const timeS = Number(view.getBigUint64(16, true)) / 1e9;

  if (type === MSG_DETECTION_REPORT) {
    if (bytes.byteLength !== HEADER_SIZE + count * DETECTION_SIZE) {
      return fail("detection count does not match datagram length");
    }
    const detections: Detection[] = [];
    for (let i = 0; i < count; i++) {
      const o = HEADER_SIZE + i * DETECTION_SIZE;
      const d = { x: view.getFloat64(o, true), y: view.getFloat64(o + 8, true) };
      if (!Number.isFinite(d.x) || !Number.isFinite(d.y)) {
        return fail("record contains NaN or infinity");
      }
      detections.push(d);
    }
    return { kind: "detections", seq, sensorId: sourceId, timeS, detections };
  }

  if (type === MSG_TRACK_REPORT) {
    if (bytes.byteLength !== HEADER_SIZE + TRACK_SUMMARY_SIZE + count * TRACK_SIZE) {
      return fail("track count does not match datagram length");
    }
    const processingUs = view.getUint32(HEADER_SIZE, true) / 1000;
    const detectionsInScan = view.getUint16(HEADER_SIZE + 4, true);
    const tracks: TrackState[] = [];
    for (let i = 0; i < count; i++) {
      const o = HEADER_SIZE + TRACK_SUMMARY_SIZE + i * TRACK_SIZE;
      const status = STATUS_BY_CODE[view.getUint8(o + 4)];
      if (status === undefined) return fail("track status is not a defined value");
      const t: TrackState = {
        id: view.getUint32(o, true),
        status,
        hits: view.getUint16(o + 6, true),
        x: view.getFloat64(o + 8, true),
        y: view.getFloat64(o + 16, true),
        vx: view.getFloat64(o + 24, true),
        vy: view.getFloat64(o + 32, true),
        posSigma: view.getFloat32(o + 40, true),
        velSigma: view.getFloat32(o + 44, true),
      };
      const numbers = [t.x, t.y, t.vx, t.vy, t.posSigma, t.velSigma];
      if (!numbers.every(Number.isFinite)) return fail("record contains NaN or infinity");
      tracks.push(t);
    }
    return { kind: "tracks", seq, sourceId, timeS, processingUs, detectionsInScan, tracks };
  }

  return fail("unknown message type");
}
