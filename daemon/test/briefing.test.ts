/** Morning briefing: feed parsing, topic filter, time/presence gate, offline mode, spoken lines. */
import { describe, it, expect } from "vitest";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { MockRobot } from "../src/presence/client.js";
import { SettingsStore } from "../src/settings.js";
import { Briefing, parseFeed } from "../src/briefing/index.js";

const NOW = new Date(2026, 9, 12, 8, 0); // the harness clock; every fixture date is relative to it
const RSS = `<?xml version="1.0"?><rss><channel><title>Feed</title>
<item><title><![CDATA[Solana ETF filing lands at the SEC]]></title><link>https://x/1</link><pubDate>${new Date(NOW.getTime() - 3600_000).toUTCString()}</pubDate></item>
<item><title>Bitcoin miners sell &amp; hedge</title><link>https://x/2</link><pubDate>${new Date(NOW.getTime() - 7200_000).toUTCString()}</pubDate></item>
<item><title>Old news about Ethereum</title><link>https://x/3</link><pubDate>${new Date(NOW.getTime() - 3 * 86400_000).toUTCString()}</pubDate></item>
<item><title>Solana ETF filing lands at the SEC</title><link>https://x/dup</link><pubDate>${NOW.toUTCString()}</pubDate></item>
</channel></rss>`;
const ATOM = `<feed xmlns="http://www.w3.org/2005/Atom"><entry><title>Jupiter ships <b>v7</b></title><link href="https://y/1"/><updated>${NOW.toISOString()}</updated></entry></feed>`;
const PRICES = { bitcoin: { usd: 112345.6, usd_24h_change: -2.34 }, solana: { usd: 187.12, usd_24h_change: 4.5 } };

const fetchImpl = (async (url: string | URL | Request) => {
  const u = String(url);
  if (u.includes("coingecko")) return new Response(JSON.stringify(PRICES), { status: 200 });
  if (u.includes("rss.test")) return new Response(RSS, { status: 200 });
  if (u.includes("atom.test")) return new Response(ATOM, { status: 200 });
  if (u.includes("down.test")) return new Response("", { status: 503 });
  throw new Error("unreachable " + u);
}) as unknown as typeof fetch;

function harness(patch: Record<string, unknown> = {}, now = NOW) {
  const store = new SettingsStore(mkdtempSync(join(tmpdir(), "pincer-brief-")));
  store.update({ briefing: { enabled: true, time: "07:30", coins: ["bitcoin", "solana"], feeds: ["https://rss.test/feed", "https://atom.test/feed"], maxHeadlines: 5, portfolio: false, ...patch } });
  const robot = new MockRobot();
  robot.set({ person: true, owner: true, voice_ok: true, duress: false, moved: false, confidence: { lidar: 1, face: 1 }, ts: Date.now() / 1000, battery: { pct: 18, voltage: 11.1, charging: false } });
  const b = new Briefing({ robot, settings: () => store.get(), fetchImpl, now: () => now, agentReport: () => ["Closed two positions, up 1.2 percent."], offlinePending: () => 1, inheritance: async () => ({ armed: true, inactivity_days: 90, days_since_touch: 80 }) });
  return { store, robot, b };
}

describe("morning briefing", () => {
  it("parses RSS and Atom, drops old and duplicate headlines", () => {
    const rss = parseFeed(RSS, "rss.test");
    expect(rss.map((h) => h.title)).toEqual(["Solana ETF filing lands at the SEC", "Bitcoin miners sell & hedge", "Old news about Ethereum", "Solana ETF filing lands at the SEC"]);
    expect(parseFeed(ATOM, "atom.test")[0]).toMatchObject({ title: "Jupiter ships v7", link: "https://y/1" });
  });

  it("collects quotes, filtered headlines and house facts; composes plain sentences", async () => {
    const { b } = harness({ topics: ["solana", "jupiter"] });
    const f = await b.collect();
    expect(f.quotes.map((q) => q.symbol)).toEqual(["BTC", "SOL"]);
    expect(f.headlines.map((h) => h.title)).toEqual(["Jupiter ships v7", "Solana ETF filing lands at the SEC"]); // newest first, topic-filtered, dedup, nothing older than a day
    expect(f.agent).toEqual(["Closed two positions, up 1.2 percent."]);
    expect(f.offlinePending).toBe(1);
    expect(f.inheritance).toEqual({ armed: true, daysLeft: 10 });
    const lines = b.compose(f);
    expect(lines[0]).toBe("Good morning.");
    expect(lines).toContain("BTC is at $112,346 down 2.3 percent on the day.");
    expect(lines).toContain("SOL is at $187.12 up 4.5 percent on the day.");
    expect(lines).toContain("atom.test: Jupiter ships v7.");
    expect(lines).toContain("1 signed payment is waiting to be sent.");
    expect(lines).toContain("Inheritance timer: 10 days left. Touch me to reset it.");
    expect(lines).toContain("My battery is at 18 percent.");
  });

  it("notes unreachable feeds instead of failing", async () => {
    const { b } = harness({ feeds: ["https://down.test/rss", "https://nowhere.test/rss", "https://rss.test/feed"], topics: [] });
    const f = await b.collect();
    expect(f.headlines.length).toBe(2);
    expect(f.notes).toEqual(expect.arrayContaining(["down.test answered 503", "nowhere.test unreachable"]));
  });

  it("offline mode fetches nothing and says so", async () => {
    const { b, store } = harness();
    store.update({ mode: "offline", brain: "lamport" });
    const f = await b.collect();
    expect(f.quotes).toEqual([]); expect(f.headlines).toEqual([]);
    expect(b.compose(f)).toContain("I'm in offline mode, so no prices or news this morning.");
  });

  it("reads once a day, at or after the set time, only with the owner in the room", async () => {
    const { b, robot } = harness({ topics: [] });
    expect(await b.tick(new Date(2026, 9, 12, 7, 0))).toBe(false);           // too early
    robot.set({ ...robot.state(), owner: false });
    expect(await b.tick(new Date(2026, 9, 12, 7, 45))).toBe(false);          // owner not there yet
    robot.set({ ...robot.state(), owner: true, ts: Date.now() / 1000 });
    expect(await b.tick(new Date(2026, 9, 12, 7, 46))).toBe(true);           // first sighting: read
    expect(robot.briefs.length).toBe(1);
    expect(robot.spoken[0]).toBe("Good morning.");
    expect(await b.tick(new Date(2026, 9, 12, 9, 0))).toBe(false);           // not twice
    expect(await b.tick(new Date(2026, 9, 13, 7, 31))).toBe(true);           // next day
    expect(await b.tick(new Date(2026, 9, 14, 15, 0))).toBe(false);          // far past the window: skipped, not read at 15:00
    expect(await b.tick(new Date(2026, 9, 14, 16, 0))).toBe(false);
  });

  it("is off by default and the settings reject bad times", () => {
    const store = new SettingsStore(mkdtempSync(join(tmpdir(), "pincer-brief-")));
    expect(store.get().briefing.enabled).toBe(false);
    expect(store.get().briefing.feeds.length).toBe(4);
    expect(() => store.update({ briefing: { time: "seven" } })).toThrow();
    store.update({ briefing: { coins: ["hyperliquid"], topics: ["hype"] } });
    expect(store.get().briefing.coins).toEqual(["hyperliquid"]);
    expect(store.get().briefing.time).toBe("07:30"); // untouched fields survive a partial patch
  });
});
