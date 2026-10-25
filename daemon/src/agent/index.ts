/**
 * The overnight agent. Works inside a budget the owner pre-authorised on the controller (an `allowance`
 * granted with a long touch before bed). Small moves it makes alone; everything else becomes a proposal
 * the robot reads out in the morning.
 *
 * Signals come from a provider behind an interface (`signals.ts`) so the product name of the feed is a
 * configuration detail, not code.
 */
import { verifyJupiterTx } from "../swap/verify.js";
import { parseMessage } from "../tx/parse.js";
import { Connection, VersionedTransaction } from "@solana/web3.js";
import type { Signer } from "../sign.js";
import type { ControllerClient } from "../controller/types.js";
import type { RobotLink } from "../presence/client.js";
import { JUPITER_V6, SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET } from "../tx/known.js";
import { decide, type Position, type Rules, type Signal, type Decision } from "./rules.js";
import { type SignalProvider, mockProvider, httpProvider } from "./signals.js";
import { jupiterSwapTx, jupiterQuote } from "./jupiter.js";
import { buildReport, type Report } from "./report.js";
import { log } from "../config.js";

export interface AgentConfig {
  budgetLamports: bigint;
  maxPerTradeLamports: bigint;
  maxOpen: number;
  stopLossPct: number;
  takeProfitPct: number;
  quietHours: string; // "00:00-08:00" local time
  signalsUrl: string;
}

const SOL = "So11111111111111111111111111111111111111112";
const ALLOWANCE_PROGRAMS = [JUPITER_V6, SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET];

export class Agent {
  /** token accounts the agent knows to be the owner's (its own positions' ATAs); anything else is a drain */
  private ownAccounts = new Set<string>();
  private positions: Position[] = [];
  private proposals: Decision[] = [];
  private actions: string[] = [];
  private report: Report | null = null;
  private provider: SignalProvider;
  private timer: NodeJS.Timeout | null = null;
  private rules: Rules;

  constructor(
    private cfg: AgentConfig,
    private signer: Signer,
    private controller: ControllerClient,
    private robot: RobotLink,
    private conn: Connection,
    private getOwner: () => string,
    provider?: SignalProvider,
  ) {
    this.provider = provider ?? (cfg.signalsUrl ? httpProvider(cfg.signalsUrl) : mockProvider());
    this.rules = { maxPerTradeLamports: cfg.maxPerTradeLamports, maxOpen: cfg.maxOpen, stopLossPct: cfg.stopLossPct, takeProfitPct: cfg.takeProfitPct, budgetLamports: cfg.budgetLamports };
  }

  status() {
    return { enabled: true, budgetLamports: this.cfg.budgetLamports, open: this.positions.length, positions: this.positions, proposals: this.proposals.length, lastActions: this.actions.slice(-10), quiet: this.inQuietHours() };
  }
  lastReport() { return this.report; }

  inQuietHours(now = new Date()): boolean {
    const [a, b] = this.cfg.quietHours.split("-");
    if (!a || !b) return false;
    const toMin = (s: string) => { const [h, m] = s.split(":").map(Number); return h * 60 + (m || 0); };
    const cur = now.getHours() * 60 + now.getMinutes();
    const start = toMin(a), end = toMin(b);
    return start <= end ? cur >= start && cur < end : cur >= start || cur < end;
  }

  /** Owner grants tonight's allowance with a long touch; the robot explains what it is granting. */
  async arm(untilUnix: number): Promise<boolean> {
    await this.robot.say(`Tonight I may spend up to ${Number(this.cfg.budgetLamports) / 1e9} SOL through Jupiter, at most ${Number(this.cfg.maxPerTradeLamports) / 1e9} per trade. Hold your hand on me for three seconds to allow it.`);
    const ok = await this.controller.allowance({ lamports: this.cfg.budgetLamports.toString(), programs: ALLOWANCE_PROGRAMS, until_unix: untilUnix, max_per_tx_lamports: this.cfg.maxPerTradeLamports.toString() });
    await this.robot.say(ok ? "Allowed. Sleep well." : "No allowance. I'll only watch tonight.");
    return ok;
  }

  async setBudget(lamports: bigint) { this.cfg.budgetLamports = lamports; this.rules.budgetLamports = lamports; return true; }

  start(intervalMs = 60_000) { this.stop(); this.timer = setInterval(() => this.tick().catch((e) => log.warn("agent tick failed", String(e))), intervalMs); }
  stop() { if (this.timer) clearInterval(this.timer); this.timer = null; }

  /** One evaluation pass. Exposed for tests and the HTTP API. */
  async tick(): Promise<{ acted: string[]; proposed: number }> {
    const signals = await this.provider.signals().catch(() => [] as Signal[]);
    const prices = await this.provider.prices(this.positions.map((p) => p.mint)).catch(() => ({} as Record<string, number>));
    for (const p of this.positions) if (prices[p.mint]) p.lastPrice = prices[p.mint];
    const decisions = decide({ signals, positions: this.positions, rules: this.rules, spentLamports: this.positions.reduce((a, p) => a + p.costLamports, 0n) });
    const acted: string[] = [];
    for (const d of decisions) {
      if (d.kind === "hold") continue;
      if (d.autonomous && this.inQuietHours()) {
        const ok = await this.execute(d);
        if (ok) acted.push(d.say);
      } else {
        this.proposals.push(d);
      }
    }
    return { acted, proposed: this.proposals.length };
  }

  private async execute(d: Decision, source: "agent" | "api" = "agent"): Promise<boolean> {
    if (d.kind === "hold") return false;
    try {
      const owner = this.getOwner();
      const [inMint, outMint, amount] = d.kind === "buy" ? [SOL, d.mint, d.lamports] : [d.mint, SOL, d.tokenAmount];
      const quote = await jupiterQuote(inMint, outMint, amount);
      const txB64 = await jupiterSwapTx(quote, owner);
      const txBytes = new Uint8Array(Buffer.from(txB64, "base64"));
      // the bytes come from a web API: before the allowance can sign them without a touch they must be exactly
      // the swap that was quoted (router, direction, amounts, min-out, destination = the owner's own account)
      const tx = VersionedTransaction.deserialize(txBytes);
      const parsed = parseMessage(tx.message.serialize(), { owner });
      verifyJupiterTx(parsed, BigInt(String(quote.otherAmountThreshold ?? quote.outAmount)), Number(quote.slippageBps ?? 0), amount);
      if (parsed.tokenOut.some((t) => t.dest && !this.ownAccounts.has(t.dest))) throw new Error("tokens would leave to an account that is not the owner's");
      const r = await this.signer.sign({ txBytes, source });
      if (!r.ok) { this.actions.push(`${d.say} → not signed (${r.reason})`); return false; }
      const sig = await this.conn.sendRawTransaction(r.signedTx, { skipPreflight: false, maxRetries: 3 });
      if (d.kind === "buy") this.positions.push({ mint: d.mint, costLamports: d.lamports, tokenAmount: BigInt(quote.outAmount), entryPrice: Number(d.lamports) / Number(quote.outAmount), lastPrice: Number(d.lamports) / Number(quote.outAmount), openedAt: Date.now(), sig });
      else this.positions = this.positions.filter((p) => p.mint !== d.mint);
      this.actions.push(`${d.say} (${sig.slice(0, 8)})`);
      return true;
    } catch (e) {
      this.actions.push(`${d.say} → failed: ${String((e as Error).message ?? e).slice(0, 80)}`);
      return false;
    }
  }

  /** Morning: speak the night's actions and ask about proposals one by one. */
  async morning(): Promise<Report> {
    this.report = buildReport({ actions: this.actions, positions: this.positions, proposals: this.proposals, budgetLamports: this.cfg.budgetLamports });
    await this.robot.morningReport(this.report.items);
    // the night is over: nothing from here on rides the allowance, every proposal takes presence and a touch
    await this.controller.allowance({ revoke: true });
    for (const p of this.proposals) {
      await this.robot.say(`${p.say}. Touch me to do it, or say no.`);
      const ok = await this.execute({ ...p, autonomous: false }, "api");
      this.actions.push(ok ? `morning: ${p.say}` : `morning: skipped ${p.say}`);
    }
    this.proposals = [];
    this.actions = [];
    return this.report;
  }
}

export { VersionedTransaction };
