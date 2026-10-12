/**
 * Morning briefing. The standing newspaper: when the owner first shows up in the morning, the robot reads
 * the night in one breath. What goes in is the owner's choice (settings.briefing): the coins to quote, the
 * topics and feeds to watch, how many headlines, what time. The daemon only gathers facts; the robot's
 * chosen brain turns them into speech in its own voice, or reads them plainly when no brain is set.
 *
 * Nothing here touches a key. In offline mode no feed or price API is called: the brief is what the house
 * already knows (agent report, pending offline payments, alerts, inheritance timer, battery).
 */
import { Connection, PublicKey } from "@solana/web3.js";
import type { RobotLink } from "../presence/client.js";
import type { Settings } from "../settings.js";
import { log } from "../config.js";

export interface BriefingSettings {
  enabled: boolean;
  /** Local time, "HH:MM". The brief is read at the first sighting of the owner at or after this time. */
  time: string;
  /** CoinGecko ids ("bitcoin", "ethereum", "solana", "jupiter-exchange-solana", …). */
  coins: string[];
  /** Keywords; a headline is kept when it mentions one (case-insensitive). Empty = keep all. */
  topics: string[];
  /** RSS/Atom feed URLs. */
  feeds: string[];
  maxHeadlines: number;
  /** Also quote the owner's SOL balance and its USD value. */
  portfolio: boolean;
  /** Only read it out when the owner is verified in the room (otherwise it waits). */
  requirePresence: boolean;
}

export const DEFAULT_FEEDS = [
  "https://www.coindesk.com/arc/outboundfeeds/rss/",
  "https://cointelegraph.com/rss",
  "https://www.theblock.co/rss.xml",
  "https://decrypt.co/feed",
];

export interface Quote { id: string; symbol: string; usd: number; change24h: number | null }
export interface Headline { title: string; source: string; link: string; at: number }
export interface Facts {
  date: string;
  quotes: Quote[];
  headlines: Headline[];
  portfolio?: { sol: number; usd: number | null };
  agent?: string[];
  offlinePending?: number;
  inheritance?: { armed: boolean; daysLeft?: number };
  battery?: { pct: number; charging: boolean };
  mode: "offline" | "online";
  /** Why some section is missing, for the brain to mention ("no network", "feed unreachable"). */
  notes: string[];
}

const SYMBOLS: Record<string, string> = { bitcoin: "BTC", ethereum: "ETH", solana: "SOL", "usd-coin": "USDC", tether: "USDT", "jupiter-exchange-solana": "JUP", binancecoin: "BNB", ripple: "XRP", dogecoin: "DOGE", cardano: "ADA", avalanche: "AVAX", chainlink: "LINK", polkadot: "DOT", "matic-network": "POL", "render-token": "RENDER", pyth: "PYTH", bonk: "BONK", dogwifcoin: "WIF", "pudgy-penguins": "PENGU", hyperliquid: "HYPE" };

export interface BriefingDeps {
  robot: RobotLink;
  settings: () => Settings;
  conn?: Connection;
  getOwner?: () => string;
  agentReport?: () => string[] | null;
  offlinePending?: () => number;
  inheritance?: () => Promise<{ armed: boolean; inactivity_days?: number; days_since_touch?: number } | null>;
  fetchImpl?: typeof fetch;
  now?: () => Date;
}

export class Briefing {
  private deliveredDay: string | null = null;
  private timer: NodeJS.Timeout | null = null;
  private fetchImpl: typeof fetch;
  private now: () => Date;
  constructor(private d: BriefingDeps) { this.fetchImpl = d.fetchImpl ?? fetch; this.now = d.now ?? (() => new Date()); }

  private cfg(): BriefingSettings { return this.d.settings().briefing; }
  private dayKey(d: Date) { return `${d.getFullYear()}-${d.getMonth() + 1}-${d.getDate()}`; }
  private minutes(d: Date) { return d.getHours() * 60 + d.getMinutes(); }

  start(intervalMs = 60_000) { this.stop(); this.timer = setInterval(() => this.tick().catch((e) => log.warn("briefing tick failed", String(e))), intervalMs); }
  stop() { if (this.timer) clearInterval(this.timer); this.timer = null; }

  /** Once a day, at or after the set time, when the owner is in the room (if required). */
  async tick(now = this.now()): Promise<boolean> {
    const c = this.cfg();
    if (!c.enabled) return false;
    const key = this.dayKey(now);
    if (this.deliveredDay === key) return false;
    const [h, m] = c.time.split(":").map(Number);
    if (this.minutes(now) < (h || 0) * 60 + (m || 0)) return false;
    // not later than noon-ish: a brief read at 17:00 is not a morning brief; it waits for tomorrow
    if (this.minutes(now) >= (h || 0) * 60 + (m || 0) + 5 * 60) { this.deliveredDay = key; return false; }
    if (c.requirePresence && !this.d.robot.ownerPresent()) return false;
    await this.deliver();
    this.deliveredDay = key;
    return true;
  }

  /** Gather everything the brief may mention. Network sections are skipped in offline mode. */
  async collect(): Promise<Facts> {
    const c = this.cfg();
    const st = this.d.settings();
    const now = this.now();
    const facts: Facts = { date: now.toDateString(), quotes: [], headlines: [], mode: st.mode, notes: [] };
    if (st.mode === "online") {
      const [quotes, headlines] = await Promise.all([this.quotes(c.coins), this.headlines(c.feeds, c.topics, c.maxHeadlines)]);
      facts.quotes = quotes.quotes; facts.headlines = headlines.headlines;
      facts.notes.push(...quotes.notes, ...headlines.notes);
    } else {
      facts.notes.push("offline mode: no prices or news were fetched");
    }
    if (c.portfolio && this.d.conn && this.d.getOwner) {
      try {
        const lamports = await this.d.conn.getBalance(new PublicKey(this.d.getOwner()), "confirmed");
        const sol = lamports / 1e9;
        const solQuote = facts.quotes.find((q) => q.id === "solana");
        facts.portfolio = { sol, usd: solQuote ? sol * solQuote.usd : null };
      } catch { facts.notes.push("balance unavailable"); }
    }
    const agent = this.d.agentReport?.() ?? null;
    if (agent && agent.length) facts.agent = agent;
    const pend = this.d.offlinePending?.() ?? 0;
    if (pend > 0) facts.offlinePending = pend;
    const inh = await this.d.inheritance?.().catch(() => null);
    if (inh?.armed) facts.inheritance = { armed: true, daysLeft: inh.inactivity_days != null && inh.days_since_touch != null ? Math.max(0, Math.ceil(inh.inactivity_days - inh.days_since_touch)) : undefined };
    const b = this.d.robot.state().battery;
    if (b) facts.battery = { pct: b.pct, charging: b.charging };
    return facts;
  }

  async quotes(ids: string[]): Promise<{ quotes: Quote[]; notes: string[] }> {
    if (!ids.length) return { quotes: [], notes: [] };
    try {
      const r = await this.fetchImpl(`https://api.coingecko.com/api/v3/simple/price?ids=${encodeURIComponent(ids.join(","))}&vs_currencies=usd&include_24hr_change=true`, { signal: AbortSignal.timeout(8000) });
      if (!r.ok) return { quotes: [], notes: [`price service answered ${r.status}`] };
      const j = (await r.json()) as Record<string, { usd?: number; usd_24h_change?: number }>;
      const quotes: Quote[] = [];
      for (const id of ids) {
        const e = j[id]; if (!e || typeof e.usd !== "number") continue;
        quotes.push({ id, symbol: SYMBOLS[id] ?? id.toUpperCase(), usd: e.usd, change24h: typeof e.usd_24h_change === "number" ? e.usd_24h_change : null });
      }
      return { quotes, notes: quotes.length < ids.length ? ["some coins had no price"] : [] };
    } catch { return { quotes: [], notes: ["price service unreachable"] }; }
  }

  async headlines(feeds: string[], topics: string[], max: number): Promise<{ headlines: Headline[]; notes: string[] }> {
    const since = this.now().getTime() - 24 * 3600_000;
    const all: Headline[] = []; const notes: string[] = [];
    await Promise.all(feeds.map(async (url) => {
      try {
        const r = await this.fetchImpl(url, { signal: AbortSignal.timeout(8000), headers: { "user-agent": "Pincer1 briefing" } });
        if (!r.ok) { notes.push(`${hostOf(url)} answered ${r.status}`); return; }
        all.push(...parseFeed(await r.text(), hostOf(url)));
      } catch { notes.push(`${hostOf(url)} unreachable`); }
    }));
    const keys = topics.map((t) => t.toLowerCase()).filter(Boolean);
    const seen = new Set<string>();
    const picked = all
      .filter((h) => h.at === 0 || h.at >= since)
      .filter((h) => !keys.length || keys.some((k) => h.title.toLowerCase().includes(k)))
      .filter((h) => { const k = h.title.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim(); if (seen.has(k)) return false; seen.add(k); return true; })
      .sort((a, b) => b.at - a.at)
      .slice(0, max);
    return { headlines: picked, notes };
  }

  /** Plain sentences, used when the robot has no brain and as the reference the brain must not contradict. */
  compose(f: Facts): string[] {
    const out: string[] = [];
    const hour = this.now().getHours();
    out.push(hour < 12 ? "Good morning." : "Here's your brief.");
    for (const q of f.quotes) {
      const ch = q.change24h == null ? "" : ` ${q.change24h >= 0 ? "up" : "down"} ${Math.abs(q.change24h).toFixed(1)} percent on the day`;
      out.push(`${q.symbol} is at ${fmtUsd(q.usd)}${ch}.`);
    }
    if (f.portfolio) out.push(`You hold ${f.portfolio.sol.toFixed(2)} SOL${f.portfolio.usd != null ? `, about ${fmtUsd(f.portfolio.usd)}` : ""}.`);
    if (f.headlines.length) {
      out.push(`${f.headlines.length} ${f.headlines.length === 1 ? "headline" : "headlines"}.`);
      for (const h of f.headlines) out.push(`${h.source}: ${h.title}.`);
    } else if (f.mode === "online") out.push("No headlines matched your topics overnight.");
    if (f.agent?.length) { out.push("From the night agent:"); out.push(...f.agent); }
    if (f.offlinePending) out.push(`${f.offlinePending} signed ${f.offlinePending === 1 ? "payment is" : "payments are"} waiting to be sent.`);
    if (f.inheritance?.armed && f.inheritance.daysLeft != null && f.inheritance.daysLeft <= 14) out.push(`Inheritance timer: ${f.inheritance.daysLeft} days left. Touch me to reset it.`);
    if (f.battery && f.battery.pct < 25 && !f.battery.charging) out.push(`My battery is at ${f.battery.pct} percent.`);
    if (f.mode === "offline") out.push("I'm in offline mode, so no prices or news this morning.");
    return out;
  }

  /** Read it. The robot's brain phrases it when one is set; otherwise the plain sentences are read. */
  async deliver(): Promise<{ facts: Facts; lines: string[] }> {
    const facts = await this.collect();
    const lines = this.compose(facts);
    await this.d.robot.attention();
    await this.d.robot.brief(facts, lines);
    log.info("briefing delivered", { quotes: facts.quotes.length, headlines: facts.headlines.length, mode: facts.mode });
    return { facts, lines };
  }
}

const fmtUsd = (n: number) => n >= 1000 ? `$${Math.round(n).toLocaleString("en-US")}` : n >= 1 ? `$${n.toFixed(2)}` : `$${n.toPrecision(3)}`;
const hostOf = (url: string) => { try { return new URL(url).hostname.replace(/^www\./, ""); } catch { return url; } };

/** Minimal RSS 2.0 / Atom reader: titles, links, dates. No external parser, no HTML kept. */
export function parseFeed(xml: string, source: string): Headline[] {
  const out: Headline[] = [];
  const items = xml.match(/<item\b[\s\S]*?<\/item>|<entry\b[\s\S]*?<\/entry>/gi) ?? [];
  for (const it of items) {
    const title = clean(tag(it, "title"));
    if (!title) continue;
    const link = tag(it, "link") || (it.match(/<link\b[^>]*href="([^"]+)"/i)?.[1] ?? "");
    const when = tag(it, "pubDate") || tag(it, "published") || tag(it, "updated") || tag(it, "dc:date");
    const at = when ? Date.parse(when.trim()) || 0 : 0;
    out.push({ title, source, link: clean(link), at });
  }
  return out;
}
function tag(s: string, name: string): string {
  const m = s.match(new RegExp(`<${name}\\b[^>]*>([\\s\\S]*?)<\\/${name}>`, "i"));
  return m ? m[1] : "";
}
function clean(s: string): string {
  return s.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1").replace(/<[^>]+>/g, "").replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, "\"").replace(/&#39;|&apos;/g, "'").replace(/&nbsp;/g, " ").replace(/\s+/g, " ").trim();
}
