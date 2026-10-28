// Display callsigns for firm tracks. The tracker identifies tracks by number; these names
// exist only in the display, to make a busy picture easier to follow.

/** Handed out in this order. The first eight were picked by hand. */
export const CALLSIGNS: readonly string[] = [
  "Red Five",
  "USCSS Nostromo",
  "Batwing",
  "HK-Aerial",
  "Arwing",
  "T-47 Snowspeeder",
  "Owlship",
  "Blackbird",
  "Millennium Falcon",
  "Serenity",
  "Rocinante",
  "Normandy",
  "Slave I",
  "Thunderbird 2",
  "Airwolf",
  "Blue Thunder",
  "Swordfish II",
  "Ebon Hawk",
  "Eagle 5",
  "Planet Express",
  "Bebop",
  "Razor Crest",
  "Ghost",
  "Valkyrie",
  "Jumper One",
  "Heart of Gold",
  "Narcissus",
  "Sulaco",
  "Event Horizon",
  "Starbug",
  "Moya",
  "Destiny",
];

/**
 * Gives each firm track a unique name for as long as it lives.
 *
 * Names are taken in pool order, and a freed name goes back to its place in that order,
 * so the earliest names are always reused first. If every name is in use, later tracks
 * get a numbered variant ("Red Five II").
 */
export class CallsignAllocator {
  private readonly byTrack = new Map<number, string>();
  private readonly inUse = new Set<string>();

  constructor(private readonly pool: readonly string[] = CALLSIGNS) {}

  /** The track's name, assigning one if it doesn't have one yet. */
  assign(trackId: number): string {
    const existing = this.byTrack.get(trackId);
    if (existing) return existing;

    for (let round = 1; ; round++) {
      for (const base of this.pool) {
        const name = round === 1 ? base : `${base} ${toRoman(round)}`;
        if (!this.inUse.has(name)) {
          this.inUse.add(name);
          this.byTrack.set(trackId, name);
          return name;
        }
      }
    }
  }

  /** Frees the track's name for reuse. */
  release(trackId: number): void {
    const name = this.byTrack.get(trackId);
    if (name === undefined) return;
    this.byTrack.delete(trackId);
    this.inUse.delete(name);
  }

  get(trackId: number): string | undefined {
    return this.byTrack.get(trackId);
  }

  clear(): void {
    this.byTrack.clear();
    this.inUse.clear();
  }

  /** Read-only view for rendering. */
  get names(): ReadonlyMap<number, string> {
    return this.byTrack;
  }
}

function toRoman(n: number): string {
  const numerals: [number, string][] = [
    [10, "X"],
    [9, "IX"],
    [5, "V"],
    [4, "IV"],
    [1, "I"],
  ];
  let out = "";
  for (const [value, symbol] of numerals) {
    while (n >= value) {
      out += symbol;
      n -= value;
    }
  }
  return out;
}
