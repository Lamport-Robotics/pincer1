import { z } from "zod";

/** "true"/"1" and "false"/"0" only: z.coerce.boolean would read "false" as true */
const strictBool = z.preprocess((v) => {
  if (v === undefined || v === "") return false;
  const t = String(v).trim().toLowerCase();
  if (t === "true" || t === "1" || t === "yes") return true;
  if (t === "false" || t === "0" || t === "no") return false;
  return v; // anything else fails the boolean check below
}, z.boolean());

const Env = z.object({
  PINCER_MODE: z.enum(["dev", "prod"]).default("dev"),
  CONTROLLER: z.string().default("sim"),
  ROBOT_WS: z.string().default("ws://127.0.0.1:7402"),
  BIND: z.string().default("0.0.0.0"),
  PORT: z.coerce.number().default(7401),
  RPC_URL: z.string().url().default("https://api.mainnet-beta.solana.com"),
  DAILY_LIMIT_SOL: z.coerce.number().default(2),
  ADDRESS_BOOK: z.string().default(""),
  ORIGIN_ALLOWLIST: z.string().default("https://pump.fun,https://jup.ag"),
  DATA_DIR: z.string().default(`${process.env.HOME ?? "."}/.pincer1`),
  AGENT_ENABLED: strictBool.default(false),
  AGENT_BUDGET_SOL: z.coerce.number().default(1),
  AGENT_MAX_PER_TRADE_SOL: z.coerce.number().default(0.2),
  AGENT_MAX_OPEN: z.coerce.number().default(3),
  AGENT_STOP_LOSS_PCT: z.coerce.number().default(25),
  AGENT_TAKE_PROFIT_PCT: z.coerce.number().default(40),
  AGENT_QUIET_HOURS: z.string().default("00:00-08:00"),
  SIGNALS_URL: z.string().default(""),
  LOG_LEVEL: z.enum(["debug", "info", "warn", "error"]).default("info"),
  // room lock: refuse below this lidar similarity to the enrolled room (0 disables)
  ROOM_MATCH_MIN: z.coerce.number().default(0.85),
  // two-person rule: SOL amount from which two enrolled owners must be in the room (0 disables)
  TWO_PERSON_SOL: z.coerce.number().default(0),
  // dead-man's switch
  INHERIT_HEIR: z.string().default(""),
  INHERIT_DAYS: z.coerce.number().default(90),
  INHERIT_WARN_DAYS: z.string().default("14,7,1"),
  NOTIFY_WEBHOOK_URL: z.string().default(""),
  // offline payments
  OFFLINE_NONCE_ACCOUNT: z.string().default(""),
  // ── multi-chain ──
  // EVM RPCs are read as EVM_RPC_<chainId> (1, 8453, 42161, 56, 137 have public defaults); see chains/evm/rpc.ts
  ETHERSCAN_API_KEY: z.string().default(""),
  DAILY_LIMIT_ETH: z.coerce.number().default(1),
  BTC_NETWORK: z.enum(["mainnet", "testnet"]).default("mainnet"),
  BTC_ESPLORA_URL: z.string().url().default("https://blockstream.info/api"),
  BTC_MAX_FEE_RATE: z.coerce.number().positive().default(200),
  BTC_FEE_TARGET_BLOCKS: z.coerce.number().int().positive().default(3),
  DAILY_LIMIT_BTC: z.coerce.number().default(0.05),
  // swaps (online mode only)
  ONEINCH_API_KEY: z.string().default(""),
  ZEROX_API_KEY: z.string().default(""),
  // THORChain inbound addresses are pinned only when two independent thornodes agree
  THORNODE_URL: z.string().url().default("https://thornode.ninerealms.com"),
  THORNODE_URL_2: z.string().url().default("https://thornode.thorchain.liquify.com"),
  SWAP_MAX_SLIPPAGE_BPS: z.coerce.number().int().min(1).max(100).default(100),
  EVM_ROUTER_ALLOWLIST: z.string().default(""),
});

export type Config = z.infer<typeof Env> & { addressBook: Record<string, string>; originAllowlist: string[]; dailyLimitLamports: bigint; dailyLimitWei: bigint; dailyLimitSats: bigint; evmRouterAllowlist: string[] };

let cached: Config | null = null;
export function config(): Config {
  if (cached) return cached;
  const e = Env.parse(process.env);
  const addressBook: Record<string, string> = {};
  for (const pair of e.ADDRESS_BOOK.split(",").map((s) => s.trim()).filter(Boolean)) {
    const i = pair.indexOf(":");
    if (i > 0) addressBook[pair.slice(i + 1).trim()] = pair.slice(0, i).trim();
  }
  cached = {
    ...e,
    addressBook,
    originAllowlist: e.ORIGIN_ALLOWLIST.split(",").map((s) => s.trim()).filter(Boolean),
    dailyLimitLamports: BigInt(Math.round(e.DAILY_LIMIT_SOL * 1e9)),
    dailyLimitWei: BigInt(Math.round(e.DAILY_LIMIT_ETH * 1e6)) * 10n ** 12n,
    dailyLimitSats: BigInt(Math.round(e.DAILY_LIMIT_BTC * 1e8)),
    evmRouterAllowlist: e.EVM_ROUTER_ALLOWLIST.split(",").map((s) => s.trim()).filter(Boolean),
  };
  return cached;
}

export const log = {
  level: (process.env.LOG_LEVEL ?? "info") as "debug" | "info" | "warn" | "error",
  debug: (...a: unknown[]) => log.level === "debug" && console.log(new Date().toISOString(), "DBG", ...a),
  info: (...a: unknown[]) => ["debug", "info"].includes(log.level) && console.log(new Date().toISOString(), "INF", ...a),
  warn: (...a: unknown[]) => log.level !== "error" && console.warn(new Date().toISOString(), "WRN", ...a),
  error: (...a: unknown[]) => console.error(new Date().toISOString(), "ERR", ...a),
};
