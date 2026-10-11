/**
 * Portfolio alerts. "If what I hold drops X%, wake me." The daemon watches prices and balances; when a
 * rule trips it escalates: the robot wakes the owner if it can see them (or knows its room and the owner
 * is home), otherwise the phone gets an alarm through ntfy / Telegram / a webhook.
 *
 * Prices: SOL and SPL tokens from Jupiter's price API, BTC from CoinGecko. Balances from the RPC.
 */
import { Connection, PublicKey } from "@solana/web3.js";
import type { RobotLink } from "../presence/client.js";
import { log } from "../config.js";

export interface AlertRule {
  id: string;
  /** "SOL", "BTC", a mint address, or "TOTAL" for the whole Solana portfolio value in USD. */
  asset: string;
  /** Trip when the value falls this many percent below the reference. */
  dropPct: number;
  /** Also trip on a rise (optional). */
  risePct?: number;
  /** "day" = vs 24 h ago; "set" = vs the value when the rule was created/reset. */
  reference: "day" | "set";
  referenceValue?: number;
  /** Wake the owner in person at night too (otherwise phone-only between quiet hours). */
  wakeAtNight: boolean;
  enabled: boolean;
  lastTripAt?: number;
}

export interface Notifier {
  ntfyTopic?: string; // https://ntfy.sh/<topic>, priority urgent = phone alarm
  telegram?: { botToken: string; chatId: string };
  webhook?: string;
}

export interface WakeLines { lines: string[] }

export const DEFAULT_WAKE_LINES = [
  "Hey. Wake up.",
  "Dude. It's in the red.",
  "Hey man. Wake up. Your bag is down {pct} percent.",
  "I'm not going to stop. {asset} is down {pct} percent. Touch me.",
  "WAKE UP. {asset}. Minus {pct}. Touch me or I keep going.",
];

export class Alerts {
  private rules: AlertRule[] = [];
  private seq = 0;
  private timer: NodeJS.Timeout | null = null;
  private cooldownMs = 30 * 60_000;
  private day: { at: number; prices: Record<string, number>; total: number } | null = null;

  constructor(
    private conn: Connection,
    private robot: RobotLink,
    private getOwner: () => string,
    private notifier: Notifier,
    private wakeEnabled: () => boolean,
    private quietHours: () => boolean,
    private fetchImpl: typeof fetch = fetch,
  ) {}

  list() { return this.rules; }
  add(r: Omit<AlertRule, "id">): AlertRule { const rule = { ...r, id: `a${++this.seq}` }; this.rules.push(rule); return rule; }
  remove(id: string) { this.rules = this.rules.filter((r) => r.id !== id); }
  reset(id: string, value: number) { const r = this.rules.find((x) => x.id === id); if (r) { r.referenceValue = value; r.lastTripAt = undefined; } }

  start(intervalMs = 60_000) { this.stop(); this.timer = setInterval(() => this.tick().catch((e) => log.warn("alerts tick failed", String(e))), intervalMs); }
  stop() { if (this.timer) clearInterval(this.timer); this.timer = null; }

  /** Current USD price per unit for the assets referenced by the rules. */
  async prices(assets: string[]): Promise<Record<string, number>> {
    const out: Record<string, number> = {};
    const sol = "So11111111111111111111111111111111111111112";
    const mints = assets.filter((a) => a !== "BTC" && a !== "TOTAL").map((a) => (a === "SOL" ? sol : a));
    if (mints.length) {
      const r = await this.fetchImpl(`https://lite-api.jup.ag/price/v2?ids=${mints.join(",")}`).catch(() => null);
      if (r?.ok) { const j: any = await r.json(); for (const m of mints) { const p = Number(j?.data?.[m]?.price); if (p > 0) out[m === sol ? "SOL" : m] = p; } }
    }
    if (assets.includes("BTC")) {
      const r = await this.fetchImpl("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd").catch(() => null);
      if (r?.ok) { const j: any = await r.json(); const p = Number(j?.bitcoin?.usd); if (p > 0) out.BTC = p; }
    }
    return out;
  }

  /** USD value of the owner's SOL + the mints referenced by rules. */
  async portfolioUsd(prices: Record<string, number>): Promise<number> {
    const owner = new PublicKey(this.getOwner());
    let total = 0;
    const lamports = await this.conn.getBalance(owner, "confirmed").catch(() => 0);
    if (prices.SOL) total += (lamports / 1e9) * prices.SOL;
    for (const [asset, price] of Object.entries(prices)) {
      if (asset === "SOL" || asset === "BTC") continue;
      try {
        const accs = await this.conn.getParsedTokenAccountsByOwner(owner, { mint: new PublicKey(asset) }, "confirmed");
        for (const a of accs.value) total += Number((a.account.data as any).parsed?.info?.tokenAmount?.uiAmount ?? 0) * price;
      } catch { /* skip */ }
    }
    return total;
  }

  async tick(now = Date.now()): Promise<{ tripped: AlertRule[] }> {
    const active = this.rules.filter((r) => r.enabled);
    if (!active.length) return { tripped: [] };
    const assets = [...new Set(active.map((r) => r.asset))];
    const prices = await this.prices(assets.filter((a) => a !== "TOTAL").concat(active.some((r) => r.asset === "TOTAL") ? ["SOL"] : []));
    const total = active.some((r) => r.asset === "TOTAL") ? await this.portfolioUsd(prices) : 0;
    if (!this.day || now - this.day.at > 24 * 3600_000) this.day = { at: now, prices: { ...prices }, total };

    const tripped: AlertRule[] = [];
    for (const r of active) {
      const cur = r.asset === "TOTAL" ? total : prices[r.asset];
      if (!cur) continue;
      if (r.reference === "set" && r.referenceValue == null) { r.referenceValue = cur; continue; }
      const ref = r.reference === "set" ? r.referenceValue! : r.asset === "TOTAL" ? this.day.total : this.day.prices[r.asset];
      if (!ref) continue;
      const pct = ((cur - ref) / ref) * 100;
      const down = pct <= -r.dropPct;
      const up = r.risePct != null && pct >= r.risePct;
      if (!down && !up) continue;
      if (r.lastTripAt && now - r.lastTripAt < this.cooldownMs) continue;
      r.lastTripAt = now;
      tripped.push(r);
      await this.escalate(r, pct, cur);
    }
    return { tripped };
  }

  private fill(line: string, asset: string, pct: number) { return line.replace("{asset}", asset === "TOTAL" ? "your portfolio" : asset).replace("{pct}", Math.abs(pct).toFixed(0)); }

  /** Robot first if it can reach the owner, phone otherwise (and phone too when the robot gives up). */
  async escalate(r: AlertRule, pct: number, value: number) {
    const label = r.asset === "TOTAL" ? "portfolio" : r.asset;
    const text = `${label} ${pct <= 0 ? "down" : "up"} ${Math.abs(pct).toFixed(1)}% (${value.toFixed(2)} USD)`;
    const p = this.robot.state();
    const night = this.quietHours();
    const canWake = this.wakeEnabled() && (!night || r.wakeAtNight) && (p.owner || (p.room_match ?? 0) >= 0.85);
    let acked = false;
    if (canWake) {
      const lines = DEFAULT_WAKE_LINES.map((l) => this.fill(l, r.asset, pct));
      acked = await this.robot.wake(text, lines, 120);
    }
    if (!acked) await this.phone(text, canWake ? "robot could not wake you" : p.owner ? "quiet hours" : "you are not home");
    log.warn("alert", { rule: r.id, text, acked });
  }

  async phone(text: string, why: string) {
    const n = this.notifier;
    const body = `Pincer1: ${text}. (${why})`;
    const jobs: Promise<unknown>[] = [];
    if (n.ntfyTopic) jobs.push(this.fetchImpl(`https://ntfy.sh/${n.ntfyTopic}`, { method: "POST", headers: { Title: "Pincer1 alarm", Priority: "urgent", Tags: "rotating_light" }, body }));
    if (n.telegram) jobs.push(this.fetchImpl(`https://api.telegram.org/bot${n.telegram.botToken}/sendMessage`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ chat_id: n.telegram.chatId, text: body }) }));
    if (n.webhook) jobs.push(this.fetchImpl(n.webhook, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ event: "alert", text, why, at: Date.now() }) }));
    await Promise.allSettled(jobs);
  }
}
