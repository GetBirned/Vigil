import { createHmac } from "node:crypto";
import { chmodSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterAll, describe, expect, it } from "vitest";

import { AUTH_TAG_SIZE, Opener, ReplayWindow, keyId, loadKeyFile } from "../server/auth.ts";

const KEY = Buffer.from(Array.from({ length: 32 }, (_, i) => i)); // 00 01 .. 1f

// The ICD's worked example, also checked by the C++ tests: epoch 1, counter 0, payload "abc".
const EXAMPLE = Buffer.from(
  "534c4756010095140100000000000000000000000000000061626363358950b813e954bcd5c07a59da7d7e",
  "hex",
);

// Seals a payload field by field from the ICD, independently of the code under test.
function seal(key: Buffer, epoch: bigint, counter: bigint, payload: string): Buffer {
  const header = Buffer.alloc(24);
  header.writeUInt32LE(0x56474c53, 0);
  header.writeUInt16LE(1, 4);
  header.writeUInt16LE(keyId(key), 6);
  header.writeBigUInt64LE(epoch, 8);
  header.writeBigUInt64LE(counter, 16);
  const signed = Buffer.concat([header, Buffer.from(payload)]);
  const tag = createHmac("sha256", key).update(signed).digest().subarray(0, AUTH_TAG_SIZE);
  return Buffer.concat([signed, tag]);
}

describe("authenticated envelope", () => {
  it("matches the ICD worked example", () => {
    expect(keyId(KEY)).toBe(0x1495);
    expect(seal(KEY, 1n, 0n, "abc").equals(EXAMPLE)).toBe(true);
    const opened = new Opener(KEY).open(EXAMPLE);
    expect(opened.ok && opened.payload.toString()).toBe("abc");
  });

  it("rejects every flipped bit without moving the replay window", () => {
    const opener = new Opener(KEY);
    for (let byte = 0; byte < EXAMPLE.length; byte++) {
      for (let bit = 0; bit < 8; bit++) {
        const bad = Buffer.from(EXAMPLE);
        bad.writeUInt8(bad.readUInt8(byte) ^ (1 << bit), byte);
        expect(opener.open(bad).ok, `byte ${byte} bit ${bit}`).toBe(false);
      }
    }
    expect(opener.open(EXAMPLE).ok).toBe(true);
  });

  it("rejects plain messages, other keys and replays", () => {
    const opener = new Opener(KEY);
    expect(opener.open(Buffer.from("VGL1 plain message bytes...")).ok).toBe(false);
    const otherKey = Buffer.alloc(32, 7);
    expect(opener.open(seal(otherKey, 1n, 0n, "fake"))).toEqual({
      ok: false,
      reason: "sealed with a different key",
    });
    expect(opener.open(EXAMPLE).ok).toBe(true);
    expect(opener.open(EXAMPLE)).toEqual({ ok: false, reason: "replayed" });
  });

  it("accepts a restarted sender and rejects its earlier run", () => {
    const opener = new Opener(KEY);
    const captured = seal(KEY, 1000n, 5n, "old");
    expect(opener.open(seal(KEY, 1000n, 0n, "a")).ok).toBe(true);
    expect(opener.open(seal(KEY, 2000n, 0n, "b")).ok).toBe(true);
    expect(opener.open(captured)).toEqual({ ok: false, reason: "from an earlier run of the sender" });
  });
});

describe("ReplayWindow", () => {
  it("allows reordering inside the window, once, and nothing older", () => {
    const w = new ReplayWindow();
    w.accept(1n, 100n);
    // The window holds the newest counter and the 63 before it.
    expect(w.check(1n, 36n)).toBe("older than the replay window");
    expect(w.check(1n, 37n)).toBeUndefined();
    w.accept(1n, 50n);
    expect(w.check(1n, 50n)).toBe("replayed");
    w.accept(1n, 100n + 64n); // slides 64: everything before falls out
    expect(w.check(1n, 101n)).toBeUndefined();
    expect(w.check(1n, 100n)).toBe("older than the replay window");
  });
});

describe("key files", () => {
  const dir = mkdtempSync(join(tmpdir(), "vigil-key-"));
  afterAll(() => rmSync(dir, { recursive: true }));

  it("loads a private key and refuses one others can read", () => {
    const path = join(dir, "vigil.key");
    writeFileSync(path, KEY.toString("hex") + "\n");
    chmodSync(path, 0o600);
    expect(loadKeyFile(path).equals(KEY)).toBe(true);
    chmodSync(path, 0o644);
    expect(() => loadKeyFile(path)).toThrow(/chmod 600/);
  });
});
