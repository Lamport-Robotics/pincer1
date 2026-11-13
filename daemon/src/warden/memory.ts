/**
 * Warden's owner memory: who the owner pays, which programs they use, and what a normal amount looks like.
 *
 * One rule shapes the whole file: trust is built only by an outbound send the owner confirmed. There is no
 * call that records an inbound transfer, so dust from a look-alike address can never teach Warden to trust it.
 *
 * Pure data, no I/O. `toJSON()` / `OwnerMemory.from()` give the adapters (SQLite on the Pincer1 daemon,
 * IndexedDB in the extension) one shape to store.
 */

export interface Counterparty {
  address: string;
  label?: string;
  /** Confirmed outbound sends to this address. */
  timesPaid: number;
  /** Lamports sent in total (SOL sends only), as a decimal string. */
  totalLamports: string;
  /** Largest single SOL send to this address, as a decimal string. */
  maxLamports: string;
  firstSeen: number;
  lastSeen: number;
}

export interface ProgramUse {
  uses: number;
  firstSeen: number;
  lastSeen: number;
}

export interface MemoryState {
  version: 1;
  counterparties: Record<string, Counterparty>;
  programs: Record<string, ProgramUse>;
  /** The most recent confirmed SOL send amounts (lamports, decimal strings), newest last. */
  recentLamports: string[];
}

export interface ConfirmedSend {
  /** Every address that received value from the owner in this transaction. */
  destinations: string[];
  /** SOL sent to the primary destination, in lamports (0 for a token-only send). */
  lamports: bigint;
  /** The primary SOL destination, when there is one. */
  solDest?: string | null;
  programs: string[];
  /** Unix seconds. */
  at: number;
}

const RECENT_MAX = 200;

/** Two different addresses that share their first and last four characters: the address-poisoning shape. */
export function looksAlike(candidate: string, known: string): boolean {
  if (candidate === known) return false;
  if (candidate.length < 32 || known.length < 32) return false;
  return candidate.slice(0, 4) === known.slice(0, 4) && candidate.slice(-4) === known.slice(-4);
}

export class OwnerMemory {
  private s: MemoryState;

  constructor(state?: MemoryState) {
    this.s = state ? structuredClone(state) : { version: 1, counterparties: {}, programs: {}, recentLamports: [] };
  }

  static from(json: unknown): OwnerMemory {
    const j = json as Partial<MemoryState> | null;
    if (!j || j.version !== 1 || typeof j.counterparties !== "object" || typeof j.programs !== "object" || !Array.isArray(j.recentLamports)) return new OwnerMemory();
    return new OwnerMemory(j as MemoryState);
  }

  toJSON(): MemoryState {
    return structuredClone(this.s);
  }

  /**
   * Call only after the owner confirmed the transaction and it was signed. This is the single way trust grows.
   */
  recordConfirmedSend(send: ConfirmedSend): void {
    for (const address of new Set(send.destinations)) {
      const c = this.s.counterparties[address] ?? { address, timesPaid: 0, totalLamports: "0", maxLamports: "0", firstSeen: send.at, lastSeen: send.at };
      c.timesPaid += 1;
      c.lastSeen = Math.max(c.lastSeen, send.at);
      if (send.solDest === address && send.lamports > 0n) {
        c.totalLamports = (BigInt(c.totalLamports) + send.lamports).toString();
        if (send.lamports > BigInt(c.maxLamports)) c.maxLamports = send.lamports.toString();
      }
      this.s.counterparties[address] = c;
    }
    for (const p of new Set(send.programs)) {
      const u = this.s.programs[p] ?? { uses: 0, firstSeen: send.at, lastSeen: send.at };
      u.uses += 1;
      u.lastSeen = Math.max(u.lastSeen, send.at);
      this.s.programs[p] = u;
    }
    if (send.lamports > 0n) {
      this.s.recentLamports.push(send.lamports.toString());
      if (this.s.recentLamports.length > RECENT_MAX) this.s.recentLamports.splice(0, this.s.recentLamports.length - RECENT_MAX);
    }
  }

  /** The owner names an address they have already paid. A label never creates a counterparty. */
  label(address: string, label: string): boolean {
    const c = this.s.counterparties[address];
    if (!c) return false;
    c.label = label;
    return true;
  }

  lookup(address: string): Counterparty | undefined {
    const c = this.s.counterparties[address];
    return c ? { ...c } : undefined;
  }

  /** A counterparty this address imitates (same first and last four characters, different address). */
  lookalikeOf(address: string): Counterparty | undefined {
    if (this.s.counterparties[address]) return undefined;
    for (const c of Object.values(this.s.counterparties)) if (looksAlike(address, c.address)) return { ...c };
    return undefined;
  }

  programUses(program: string): number {
    return this.s.programs[program]?.uses ?? 0;
  }

  /** What a SOL send from this owner normally looks like. `n` is how many sends it rests on. */
  typicalLamports(): { n: number; median: bigint; max: bigint } {
    const a = this.s.recentLamports.map((x) => BigInt(x)).sort((x, y) => (x < y ? -1 : x > y ? 1 : 0));
    if (a.length === 0) return { n: 0, median: 0n, max: 0n };
    return { n: a.length, median: a[Math.floor(a.length / 2)]!, max: a[a.length - 1]! };
  }
}
