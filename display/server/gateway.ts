// Display gateway: listens for the tracker's UDP display feed, pairs each scan's
// detections with the track picture they produced, and pushes JSON frames to browsers
// over WebSocket. With --static DIR it also serves the built web app.

import { createSocket } from "node:dgram";
import { readFile } from "node:fs/promises";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { extname, join, resolve, sep } from "node:path";
import { parseArgs } from "node:util";
import { WebSocket, WebSocketServer } from "ws";

import { decodeDatagram, type Detection, type Frame } from "../src/shared/protocol.ts";

const { values: args } = parseArgs({
  options: {
    "udp-port": { type: "string", default: "50001" },
    "ws-port": { type: "string", default: process.env.VIGIL_WS_PORT ?? "8081" },
    static: { type: "string" },
  },
});
const udpPort = Number(args["udp-port"]);
const wsPort = Number(args["ws-port"]);
const staticDir = args.static ? resolve(args.static) : undefined;

// Drop frames for a browser that has fallen this far behind rather than queue forever.
const MAX_BUFFERED_BYTES = 1 << 20;

// ---------------------------------------------------------------- HTTP + WebSocket

const CONTENT_TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".ico": "image/x-icon",
  ".json": "application/json",
};

async function serveStatic(req: IncomingMessage, res: ServerResponse): Promise<void> {
  if (!staticDir) {
    res.writeHead(404).end("vigil gateway: WebSocket at /ws (run with --static dist to serve the app)");
    return;
  }
  const urlPath = decodeURIComponent(new URL(req.url ?? "/", "http://x").pathname);
  let file = resolve(join(staticDir, urlPath));
  if (file !== staticDir && !file.startsWith(staticDir + sep)) {
    res.writeHead(403).end();
    return;
  }
  if (urlPath.endsWith("/")) file = join(file, "index.html");

  try {
    const body = await readFile(file);
    res.writeHead(200, { "content-type": CONTENT_TYPES[extname(file)] ?? "application/octet-stream" });
    res.end(body);
  } catch {
    // Single-page app: unknown paths get the app shell.
    const body = await readFile(join(staticDir, "index.html"));
    res.writeHead(200, { "content-type": CONTENT_TYPES[".html"] });
    res.end(body);
  }
}

const http = createServer((req, res) => {
  serveStatic(req, res).catch(() => res.writeHead(500).end());
});
const wss = new WebSocketServer({ server: http, path: "/ws" });

let lastFrame: Frame | undefined;

wss.on("connection", (ws) => {
  // A new viewer sees the current picture immediately instead of waiting a scan.
  if (lastFrame) ws.send(JSON.stringify(lastFrame));
});

function broadcast(frame: Frame): void {
  const json = JSON.stringify(frame);
  for (const client of wss.clients) {
    if (client.readyState === WebSocket.OPEN && client.bufferedAmount < MAX_BUFFERED_BYTES) {
      client.send(json);
    }
  }
}

// ---------------------------------------------------------------- UDP feed

// The tracker sends a scan's detections, then the track report for the same scan.
let pending: { seq: number; detections: Detection[] } | undefined;
let malformed = 0;

const udp = createSocket("udp4");

udp.on("message", (msg) => {
  const decoded = decodeDatagram(msg);
  switch (decoded.kind) {
    case "error":
      malformed++;
      if (malformed === 1 || malformed % 100 === 0) {
        console.warn(`gateway: dropped malformed datagram (${decoded.reason}); ${malformed} so far`);
      }
      return;
    case "detections":
      pending = { seq: decoded.seq, detections: decoded.detections };
      return;
    case "tracks": {
      const detections = pending?.seq === decoded.seq ? pending.detections : [];
      pending = undefined;
      lastFrame = {
        type: "frame",
        seq: decoded.seq,
        timeS: decoded.timeS,
        processingUs: decoded.processingUs,
        detections,
        tracks: decoded.tracks,
      };
      broadcast(lastFrame);
      return;
    }
  }
});

udp.on("error", (err) => {
  console.error(`gateway: UDP error: ${err.message}`);
  process.exit(1);
});

udp.bind(udpPort, "0.0.0.0", () => {
  console.log(`gateway: listening for the tracker's display feed on udp/${udpPort}`);
});

http.listen(wsPort, () => {
  console.log(`gateway: WebSocket on ws://localhost:${wsPort}/ws`);
  if (staticDir) console.log(`gateway: serving ${staticDir} at http://localhost:${wsPort}/`);
});

function shutdown(): void {
  udp.close();
  for (const client of wss.clients) client.terminate();
  http.close(() => process.exit(0));
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
