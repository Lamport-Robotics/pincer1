/** Signal providers. The product name of the real feed is a config value (SIGNALS_URL), not code. */
import type { Signal } from "./rules.js";

export interface SignalProvider {
  signals(): Promise<Signal[]>;
  /** Current price per token unit in lamports, for the given mints. */
  prices(mints: string[]): Promise<Record<string, number>>;
}

/** Deterministic mock for laptops and tests. */
export function mockProvider(seed: Signal[] = []): SignalProvider {
  let prices: Record<string, number> = {};
  return {
    async signals() { return seed; },
    async prices(mints) { const out: Record<string, number> = {}; for (const m of mints) out[m] = prices[m] ?? 1; return out; },
    // test seam
    ...( { setPrice(m: string, p: number) { prices[m] = p; }, setSignals(s: Signal[]) { seed = s; } } as any),
  };
}

/** HTTP feed: GET {url}/signals → Signal[]; GET {url}/prices?mints=a,b → {mint: lamportsPerUnit}. */
export function httpProvider(url: string): SignalProvider {
  const base = url.replace(/\/$/, "");
  return {
    async signals() { const r = await fetch(`${base}/signals`); if (!r.ok) throw new Error(`signals ${r.status}`); return (await r.json()) as Signal[]; },
    async prices(mints) { if (!mints.length) return {}; const r = await fetch(`${base}/prices?mints=${mints.join(",")}`); if (!r.ok) throw new Error(`prices ${r.status}`); return (await r.json()) as Record<string, number>; },
  };
}
