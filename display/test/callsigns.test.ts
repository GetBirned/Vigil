import { describe, expect, it } from "vitest";

import { CALLSIGNS, CallsignAllocator } from "../src/shared/callsigns.ts";

describe("CallsignAllocator", () => {
  it("hands out names in pool order", () => {
    const a = new CallsignAllocator();
    expect(a.assign(6)).toBe("Red Five");
    expect(a.assign(2)).toBe("USCSS Nostromo");
    expect(a.assign(9)).toBe("Batwing");
  });

  it("keeps a track's name stable", () => {
    const a = new CallsignAllocator();
    a.assign(1);
    expect(a.assign(4)).toBe("USCSS Nostromo");
    expect(a.assign(4)).toBe("USCSS Nostromo");
    expect(a.get(4)).toBe("USCSS Nostromo");
  });

  it("reuses a released name before moving down the list", () => {
    const a = new CallsignAllocator();
    a.assign(1); // Red Five
    a.assign(2); // USCSS Nostromo
    a.assign(3); // Batwing
    a.release(1);
    expect(a.get(1)).toBeUndefined();
    expect(a.assign(7)).toBe("Red Five");
    expect(a.assign(8)).toBe("HK-Aerial");
  });

  it("never gives two live tracks the same name, even past the end of the pool", () => {
    const a = new CallsignAllocator(["Alpha", "Bravo"]);
    const names = [1, 2, 3, 4, 5].map((id) => a.assign(id));
    expect(names).toEqual(["Alpha", "Bravo", "Alpha II", "Bravo II", "Alpha III"]);
    expect(new Set(names).size).toBe(names.length);
  });

  it("has no duplicate names in the default pool", () => {
    expect(new Set(CALLSIGNS).size).toBe(CALLSIGNS.length);
  });

  it("clear frees every name", () => {
    const a = new CallsignAllocator();
    a.assign(1);
    a.assign(2);
    a.clear();
    expect(a.assign(3)).toBe("Red Five");
  });
});
