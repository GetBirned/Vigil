// Authenticated datagrams: the receiving side of the envelope described in the Security
// section of docs/ICD.md, mirroring common/src/auth.cpp.

import { createHmac, timingSafeEqual } from "node:crypto";
import { readFileSync, statSync } from "node:fs";

export const AUTH_MAGIC = 0x56474c53; // "VGLS"
export const AUTH_VERSION = 1;
export const AUTH_HEADER_SIZE = 24;
export const AUTH_TAG_SIZE = 16;
export const KEY_SIZE = 32;

export type AuthError =
  | "too short for an authenticated envelope"
  | "not authenticated"
  | "unsupported envelope version"
  | "sealed with a different key"
  | "from an earlier run of the sender"
  | "older than the replay window"
  | "replayed"
  | "authentication tag does not match";

export type OpenResult = { ok: true; payload: Buffer } | { ok: false; reason: AuthError };

/** 16-bit fingerprint of a key: the first two bytes of HMAC(key, "vigil key id"). */
export function keyId(key: Buffer): number {
  return createHmac("sha256", key).update("vigil key id").digest().readUInt16LE(0);
}

/** Reads a key file of 64 hex digits. Refuses a file that other users can read or write. */
export function loadKeyFile(path: string): Buffer {
  if ((statSync(path).mode & 0o077) !== 0) {
    throw new Error(`key file ${path} is accessible by other users; run: chmod 600 ${path}`);
  }
  const hex = readFileSync(path, "utf8").replace(/\s+/g, "");
  if (!/^[0-9a-fA-F]{64}$/.test(hex)) {
    throw new Error(`key file ${path} must hold exactly 64 hex digits (openssl rand -hex 32)`);
  }
  return Buffer.from(hex, "hex");
}

/** IPsec-style sliding window: each (epoch, counter) is accepted once. */
export class ReplayWindow {
  static readonly WIDTH = 64n;
  private started = false;
  private epoch = 0n;
  private top = 0n;
  private seen = 0n; // bit i set: counter top - i has been accepted

  check(epoch: bigint, counter: bigint): AuthError | undefined {
    if (!this.started || epoch > this.epoch || (epoch === this.epoch && counter > this.top)) {
      return undefined;
    }
    if (epoch < this.epoch) return "from an earlier run of the sender";
    const behind = this.top - counter;
    if (behind >= ReplayWindow.WIDTH) return "older than the replay window";
    if (((this.seen >> behind) & 1n) !== 0n) return "replayed";
    return undefined;
  }

  /** Call only after check() passed and the tag verified. */
  accept(epoch: bigint, counter: bigint): void {
    if (!this.started || epoch > this.epoch) {
      this.started = true;
      this.epoch = epoch;
      this.top = counter;
      this.seen = 1n;
    } else if (counter > this.top) {
      const ahead = counter - this.top;
      this.seen = ahead >= ReplayWindow.WIDTH ? 1n : ((this.seen << ahead) | 1n) & 0xffff_ffff_ffff_ffffn;
      this.top = counter;
    } else {
      this.seen |= 1n << (this.top - counter);
    }
  }
}

export class Opener {
  private readonly id: number;
  private readonly window = new ReplayWindow();

  constructor(private readonly key: Buffer) {
    this.id = keyId(key);
  }

  open(datagram: Buffer): OpenResult {
    if (datagram.length >= 4 && datagram.readUInt32LE(0) !== AUTH_MAGIC) {
      return { ok: false, reason: "not authenticated" };
    }
    if (datagram.length < AUTH_HEADER_SIZE + AUTH_TAG_SIZE) {
      return { ok: false, reason: "too short for an authenticated envelope" };
    }
    if (datagram.readUInt16LE(4) !== AUTH_VERSION) return { ok: false, reason: "unsupported envelope version" };
    if (datagram.readUInt16LE(6) !== this.id) return { ok: false, reason: "sealed with a different key" };

    const epoch = datagram.readBigUInt64LE(8);
    const counter = datagram.readBigUInt64LE(16);
    const stale = this.window.check(epoch, counter);
    if (stale) return { ok: false, reason: stale };

    const signedEnd = datagram.length - AUTH_TAG_SIZE;
    const expected = createHmac("sha256", this.key)
      .update(datagram.subarray(0, signedEnd))
      .digest()
      .subarray(0, AUTH_TAG_SIZE);
    // Constant-time comparison, as on the C++ side.
    if (!timingSafeEqual(expected, datagram.subarray(signedEnd))) {
      return { ok: false, reason: "authentication tag does not match" };
    }

    this.window.accept(epoch, counter);
    return { ok: true, payload: datagram.subarray(AUTH_HEADER_SIZE, signedEnd) };
  }
}
