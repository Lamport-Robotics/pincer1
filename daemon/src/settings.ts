/**
 * Owner-facing feature toggles. Every optional behaviour is off until the owner turns it on; the only
 * fixed rule is presence (no signature without the owner in the room) in prod mode.
 *
 * Stored as JSON in DATA_DIR/settings.json, editable through GET/POST /settings. Environment variables
 * seed the defaults on first run.
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { z } from "zod";

export const SettingsSchema = z.object({
  /** Room lock: refuse signatures when the lidar does not recognise the enrolled room. */
  roomLock: z.boolean().default(false),
  roomMatchMin: z.number().min(0).max(1).default(0.85),
  /** Two-person rule: transfers at or above this many SOL need two enrolled owners in the room. 0 = off. */
  twoPersonSol: z.number().min(0).default(0),
  /** Dead-man's switch: armed on the controller via /inheritance/arm; this only records the owner's intent. */
  inheritance: z.object({ enabled: z.boolean().default(false), heir: z.string().default(""), inactivityDays: z.number().int().min(7).default(90), warnDays: z.array(z.number().int().positive()).default([14, 7, 1]), resignPromptPct: z.number().default(20) }).default({}),
  /** Air-gap relay: off = radio always on; strict = radio only while the owner is present; night = also during the agent's window. */
  airgap: z.enum(["off", "strict", "night"]).default("off"),
  /** Offline payments: allow signing without broadcasting (durable nonce). */
  offlinePayments: z.boolean().default(false),
  /** Overnight agent. */
  agent: z.boolean().default(false),
  /** Automatic charging dock (needs the INA226, pogo contacts and a registry entry named "dock"). */
  dock: z.boolean().default(false),
  /** Spoken explanations before every signature (turning this off still requires the touch). */
  speakBeforeSign: z.boolean().default(true),
  /**
   * offline: no swaps, no cloud brain, nothing leaves the house except the broadcast the owner asks for.
   * online: swaps through allowlisted routers and cloud brains are available.
   */
  mode: z.enum(["offline", "online"]).default("online"),
  /** Which brain answers the owner: the on-robot one or a cloud model. Cloud brains need `mode: online`. */
  brain: z.enum(["lamport", "claude", "openai", "deepseek"]).default("lamport"),
  /** Tolerate unlimited ERC-20 approvals when the spender is an allowlisted router. Off: exact amounts only. */
  unlimitedApproveToRouters: z.boolean().default(false),
  /** Swap slippage cap in basis points; requests above it are clamped. Never above 100 (1%). */
  swapMaxSlippageBps: z.number().int().min(1).max(100).default(100),
  /** Morning briefing: prices, headlines and house news, read at the first sighting after `time`. */
  briefing: z.object({
    enabled: z.boolean().default(false),
    time: z.string().regex(/^\d{1,2}:\d{2}$/).default("07:30"),
    coins: z.array(z.string().min(1)).max(12).default(["bitcoin", "ethereum", "solana"]),
    topics: z.array(z.string().min(1)).max(24).default([]),
    feeds: z.array(z.string().url()).max(12).default(["https://www.coindesk.com/arc/outboundfeeds/rss/", "https://cointelegraph.com/rss", "https://www.theblock.co/rss.xml", "https://decrypt.co/feed"]),
    maxHeadlines: z.number().int().min(1).max(12).default(5),
    portfolio: z.boolean().default(true),
    requirePresence: z.boolean().default(true),
  }).default({}),
});
export type Settings = z.infer<typeof SettingsSchema>;

export class SettingsError extends Error {
  constructor(readonly code: string, message: string) { super(message); }
}

export const isCloudBrain = (b: Settings["brain"]) => b !== "lamport";

export class SettingsStore {
  private cur: Settings;
  private path: string;
  constructor(dataDir: string, seed: Partial<Settings> = {}) {
    mkdirSync(dataDir, { recursive: true });
    this.path = join(dataDir, "settings.json");
    let fromDisk: unknown = {};
    try { if (existsSync(this.path)) fromDisk = JSON.parse(readFileSync(this.path, "utf8")); } catch { fromDisk = {}; }
    this.cur = SettingsSchema.parse({ ...seed, ...(fromDisk as object) });
  }
  get(): Settings { return this.cur; }
  update(patch: unknown): Settings {
    const merged = SettingsSchema.parse({ ...this.cur, ...(patch as object), inheritance: { ...this.cur.inheritance, ...((patch as any)?.inheritance ?? {}) }, briefing: { ...this.cur.briefing, ...((patch as any)?.briefing ?? {}) } });
    if (merged.mode === "offline" && isCloudBrain(merged.brain)) {
      throw new SettingsError("offline_mode", `the ${merged.brain} brain needs the network; in offline mode only the on-robot brain (lamport) is available. Switch mode to online first, or pick lamport.`);
    }
    this.cur = merged;
    writeFileSync(this.path, JSON.stringify(merged, null, 2), { mode: 0o600 });
    return merged;
  }
}
