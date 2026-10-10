import { describe, it, expect } from "vitest";
import { decide, pnlPct, type Position, type Rules } from "../src/agent/rules.js";
import { buildReport } from "../src/agent/report.js";

const rules: Rules = { budgetLamports: 1_000_000_000n, maxPerTradeLamports: 200_000_000n, maxOpen: 2, stopLossPct: 25, takeProfitPct: 40 };
const pos = (mint: string, entry: number, last: number): Position => ({ mint, costLamports: 200_000_000n, tokenAmount: 1000n, entryPrice: entry, lastPrice: last, openedAt: 0 });

describe("agent rules", () => {
  it("sells on stop loss and take profit", () => {
    const d = decide({ signals: [], positions: [pos("A".repeat(44), 100, 70), pos("B".repeat(44), 100, 150), pos("C".repeat(44), 100, 110)], rules, spentLamports: 600_000_000n });
    expect(d.map((x) => x.kind)).toEqual(["sell", "sell"]);
    expect(d[0].say).toMatch(/down 30%, stop loss/);
    expect(d[1].say).toMatch(/up 50%, taking profit/);
  });

  it("buys strong signals alone, proposes medium ones, ignores weak ones, respects budget and max open", () => {
    const signals = [
      { mint: "S".repeat(44), score: 0.95, reason: "three smart wallets bought in the last hour" },
      { mint: "M".repeat(44), score: 0.6, reason: "momentum" },
      { mint: "W".repeat(44), score: 0.2, reason: "noise" },
      { mint: "T".repeat(44), score: 0.9, reason: "strong" },
      { mint: "U".repeat(44), score: 0.85, reason: "strong but no slot" },
    ];
    const d = decide({ signals, positions: [], rules, spentLamports: 0n });
    const buys = d.filter((x) => x.kind === "buy");
    expect(buys.map((b) => [b.mint[0], b.autonomous])).toEqual([["S", true], ["T", true], ["U", false], ["M", false]]);
    expect(buys[0].kind === "buy" && buys[0].lamports).toBe(200_000_000n);
  });

  it("caps the last trade at the remaining budget", () => {
    const d = decide({ signals: [{ mint: "S".repeat(44), score: 0.95, reason: "x" }], positions: [], rules, spentLamports: 950_000_000n });
    expect(d[0].kind === "buy" && d[0].lamports).toBe(50_000_000n);
  });

  it("pnlPct", () => { expect(pnlPct(pos("A", 100, 125))).toBe(25); });
});

describe("morning report", () => {
  it("reads like a newspaper", () => {
    const r = buildReport({ actions: ["buy 0.2 SOL of PEPE: smart money inflow (abc123)"], positions: [pos("A".repeat(44), 100, 118)], proposals: [{ kind: "buy", mint: "B".repeat(44), lamports: 1n, say: "buy", autonomous: false }], budgetLamports: 1_000_000_000n });
    expect(r.items[1]).toBe("1 move overnight.");
    expect(r.items[2]).toMatch(/^buy 0.2 SOL of PEPE/);
    expect(r.items[3]).toMatch(/^Open: AAAA…AAAA up 18%\. 0\.20 SOL at work\.$/);
    expect(r.items[4]).toBe("1 thing I want to ask you about.");
    const quiet = buildReport({ actions: [], positions: [], proposals: [], budgetLamports: 0n });
    expect(quiet.items).toContain("Quiet night. I didn't trade.");
  });
});
