import { describe, expect, it } from "vitest";

import {
  DETECTION_SIZE,
  HEADER_SIZE,
  MAGIC,
  MSG_DETECTION_REPORT,
  MSG_TRACK_REPORT,
  TRACK_SIZE,
  TRACK_SUMMARY_SIZE,
  decodeDatagram,
} from "../src/shared/protocol.ts";

// Builds datagrams field by field from the ICD tables, independently of the decoder.
function header(view: DataView, type: number, seq: number, count: number, timeNs: bigint) {
  view.setUint32(0, MAGIC, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, type, true);
  view.setUint32(8, seq, true);
  view.setUint16(12, 7, true);
  view.setUint16(14, count, true);
  view.setBigUint64(16, timeNs, true);
}

function detectionReport(points: [number, number][]): Uint8Array {
  const bytes = new Uint8Array(HEADER_SIZE + points.length * DETECTION_SIZE);
  const view = new DataView(bytes.buffer);
  header(view, MSG_DETECTION_REPORT, 0xdeadbeef, points.length, 2_500_000_000n);
  points.forEach(([x, y], i) => {
    view.setFloat64(HEADER_SIZE + i * DETECTION_SIZE, x, true);
    view.setFloat64(HEADER_SIZE + i * DETECTION_SIZE + 8, y, true);
  });
  return bytes;
}

function trackReport(statusCode = 1): Uint8Array {
  const bytes = new Uint8Array(HEADER_SIZE + TRACK_SUMMARY_SIZE + TRACK_SIZE);
  const view = new DataView(bytes.buffer);
  header(view, MSG_TRACK_REPORT, 42, 1, 3_000_000_000n);
  view.setUint32(HEADER_SIZE, 12_345, true); // processing ns
  view.setUint16(HEADER_SIZE + 4, 9, true); // detections in scan
  const o = HEADER_SIZE + TRACK_SUMMARY_SIZE;
  view.setUint32(o, 6, true);
  view.setUint8(o + 4, statusCode);
  view.setUint16(o + 6, 17, true);
  view.setFloat64(o + 8, 9569.5, true);
  view.setFloat64(o + 16, -8602.25, true);
  view.setFloat64(o + 24, -45, true);
  view.setFloat64(o + 32, -85.5, true);
  view.setFloat32(o + 40, 14.5, true);
  view.setFloat32(o + 44, 6.25, true);
  return bytes;
}

describe("decodeDatagram", () => {
  it("matches the ICD's worked example bytes", () => {
    // Section 7 of docs/ICD.md, with a zero timestamp.
    const hex =
      "31 4C 47 56 01 00 01 00 EF BE AD DE 07 00 01 00 00 00 00 00 00 00 00 00" +
      " 00 00 00 00 00 00 F8 3F 00 00 00 00 00 00 02 C0";
    const bytes = Uint8Array.from(hex.split(" ").map((b) => parseInt(b, 16)));
    const out = decodeDatagram(bytes);
    expect(out).toEqual({
      kind: "detections",
      seq: 0xdeadbeef,
      sensorId: 7,
      timeS: 0,
      detections: [{ x: 1.5, y: -2.25 }],
    });
  });

  it("decodes a detection report", () => {
    const out = decodeDatagram(detectionReport([[100, 200], [-5, 6]]));
    expect(out.kind).toBe("detections");
    if (out.kind !== "detections") return;
    expect(out.timeS).toBe(2.5);
    expect(out.detections).toEqual([{ x: 100, y: 200 }, { x: -5, y: 6 }]);
  });

  it("decodes a track report", () => {
    const out = decodeDatagram(trackReport());
    expect(out).toEqual({
      kind: "tracks",
      seq: 42,
      sourceId: 7,
      timeS: 3,
      processingUs: 12.345,
      detectionsInScan: 9,
      tracks: [
        {
          id: 6,
          status: "confirmed",
          hits: 17,
          x: 9569.5,
          y: -8602.25,
          vx: -45,
          vy: -85.5,
          posSigma: 14.5,
          velSigma: 6.25,
        },
      ],
    });
  });

  it("rejects malformed datagrams", () => {
    expect(decodeDatagram(new Uint8Array(10)).kind).toBe("error");

    const badMagic = detectionReport([[1, 2]]);
    badMagic[0] = 0;
    expect(decodeDatagram(badMagic)).toMatchObject({ reason: "bad magic number" });

    const truncated = detectionReport([[1, 2]]).subarray(0, HEADER_SIZE + 8);
    expect(decodeDatagram(truncated)).toMatchObject({ kind: "error" });

    expect(decodeDatagram(trackReport(9))).toMatchObject({
      reason: "track status is not a defined value",
    });

    expect(decodeDatagram(detectionReport([[NaN, 0]]))).toMatchObject({ kind: "error" });
  });
});
