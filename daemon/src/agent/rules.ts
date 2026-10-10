/** Pure decision rules for the overnight agent. No I/O, fully unit-tested. */

export interface Signal {
  mint: string;
  symbol?: string;
  /** 0–1 strength; the provider decides how to compute it (smart-money inflow, momentum, …). */
  score: number;
  /** Human reason, spoken in the report. */
  reason: string;
  priceLamportsPerToken?: number;
}

export interface Position {
  mint: string;
  costLamports: bigint;
  tokenAmount: bigint;
  entryPrice: number; // lamports per token unit
  lastPrice: number;
  openedAt: number;
  sig?: string;
}

export interface Rules {
  budgetLamports: bigint;
  maxPerTradeLamports: bigint;
  maxOpen: number;
  stopLossPct: number;
  takeProfitPct: number;
  /** Signals below this never trade alone; between this and `proposeScore` they become morning proposals. */
  autoScore?: number;
  proposeScore?: number;
}

export type Decision =
  | { kind: "buy"; mint: string; lamports: bigint; say: string; autonomous: boolean }
  | { kind: "sell"; mint: string; tokenAmount: bigint; say: string; autonomous: boolean }
  | { kind: "hold"; mint: string; say: string; autonomous: false };

export function pnlPct(p: Position): number {
  if (!p.entryPrice) return 0;
  return ((p.lastPrice - p.entryPrice) / p.entryPrice) * 100;
}

export function decide(input: { signals: Signal[]; positions: Position[]; rules: Rules; spentLamports: bigint }): Decision[] {
  const { signals, positions, rules } = input;
  const autoScore = rules.autoScore ?? 0.8;
  const proposeScore = rules.proposeScore ?? 0.5;
  const out: Decision[] = [];

  // exits first: stop loss / take profit on open positions
  for (const p of positions) {
    const pct = pnlPct(p);
    if (pct <= -rules.stopLossPct) out.push({ kind: "sell", mint: p.mint, tokenAmount: p.tokenAmount, say: `sell ${short(p.mint)}: down ${Math.abs(pct).toFixed(0)}%, stop loss`, autonomous: true });
    else if (pct >= rules.takeProfitPct) out.push({ kind: "sell", mint: p.mint, tokenAmount: p.tokenAmount, say: `sell ${short(p.mint)}: up ${pct.toFixed(0)}%, taking profit`, autonomous: true });
  }

  // entries
  let spent = input.spentLamports;
  let open = positions.length;
  const held = new Set(positions.map((p) => p.mint));
  for (const s of [...signals].sort((a, b) => b.score - a.score)) {
    if (held.has(s.mint) || s.score < proposeScore) continue;
    const remaining = rules.budgetLamports - spent;
    if (remaining <= 0n) break;
    const size = remaining < rules.maxPerTradeLamports ? remaining : rules.maxPerTradeLamports;
    const autonomous = s.score >= autoScore && open < rules.maxOpen;
    out.push({ kind: "buy", mint: s.mint, lamports: size, say: `buy ${Number(size) / 1e9} SOL of ${s.symbol ?? short(s.mint)}: ${s.reason}`, autonomous });
    if (autonomous) { spent += size; open += 1; held.add(s.mint); }
  }
  return out;
}

export const short = (a: string) => `${a.slice(0, 4)}…${a.slice(-4)}`;
