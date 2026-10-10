/**
 * Turns a ParsedTx into the sentences the robot says out loud. Short, concrete, no hex.
 */
import { KNOWN_MINTS, PROGRAM_NAMES, JUPITER_V6, PUMP, PUMPSWAP, RAYDIUM_AMM, ORCA_WHIRLPOOL, METEORA_DLMM, RAYDIUM_CLMM, SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET, MEMO, MEMO_V1 } from "./known.js";
import type { ParsedTx } from "./parse.js";
import type { AddressIntel } from "../risk/intel.js";

export const short = (a: string, n = 4) => (a.length > 2 * n + 1 ? `${a.slice(0, n)}…${a.slice(-n)}` : a);

export function fmtSol(lamports: bigint): string {
  const sol = Number(lamports) / 1e9;
  if (sol === 0) return "0 SOL";
  if (sol < 0.001) return `${lamports} lamports`;
  return `${sol.toLocaleString("en-US", { maximumFractionDigits: sol < 1 ? 4 : 3 })} SOL`;
}

export function fmtToken(amount: bigint, mint: string | null, meta?: { symbol?: string; decimals?: number }): string {
  const known = mint ? KNOWN_MINTS[mint] : undefined;
  const decimals = meta?.decimals ?? known?.decimals;
  const symbol = meta?.symbol ?? known?.symbol ?? (mint ? `token ${short(mint)}` : "tokens");
  if (decimals == null) return `${amount.toString()} units of ${symbol}`;
  const v = Number(amount) / 10 ** decimals;
  return `${v.toLocaleString("en-US", { maximumFractionDigits: v < 1 ? 6 : 2 })} ${symbol}`;
}

const SWAP_PROGRAMS = new Set([JUPITER_V6, PUMP, PUMPSWAP, RAYDIUM_AMM, RAYDIUM_CLMM, ORCA_WHIRLPOOL, METEORA_DLMM]);
const PLUMBING = new Set([SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET, MEMO, MEMO_V1]);

export interface ExplainContext {
  origin?: string;
  intel?: Record<string, AddressIntel>; // by address
  addressBook?: Record<string, string>; // pubkey → label
  tokenMeta?: Record<string, { symbol?: string; decimals?: number }>; // mint → meta
}

export interface Explanation {
  /** One or two sentences for speech. */
  spoken: string;
  /** Bullet lines for the popup/UI. */
  lines: string[];
}

export function explain(p: ParsedTx, ctx: ExplainContext = {}): Explanation {
  const lines: string[] = [];
  const who = (a: string) => {
    const label = ctx.addressBook?.[a];
    if (label) return label;
    const i = ctx.intel?.[a];
    if (i?.isProgram) return `program ${short(a)}`;
    if (i && i.ageDays != null) {
      const age = i.ageDays < 1 ? "created today" : i.ageDays < 30 ? `${Math.floor(i.ageDays)} days old` : `${Math.floor(i.ageDays / 30)} months old`;
      return `${short(a)}, a wallet ${age} with ${i.txCount}${i.txCountIsLowerBound ? "+" : ""} transactions`;
    }
    return short(a);
  };

  const apps = p.programs.filter((x) => !PLUMBING.has(x));
  const appNames = apps.map((x) => PROGRAM_NAMES[x] ?? `unknown program ${short(x)}`);

  if (p.lamportsOut > 0n && p.dest) {
    const n = p.tokenOut.length ? "also " : "";
    lines.push(`${n}sends ${fmtSol(p.lamportsOut)} to ${who(p.dest)}`);
  }
  for (const t of p.tokenOut) {
    lines.push(`sends ${fmtToken(t.amount, t.mint, t.mint ? ctx.tokenMeta?.[t.mint] : undefined)} to ${who(t.dest)}`);
  }
  for (const t of p.tokenIn) {
    lines.push(`receives ${fmtToken(t.amount, t.mint, t.mint ? ctx.tokenMeta?.[t.mint] : undefined)}`);
  }
  if (apps.some((x) => SWAP_PROGRAMS.has(x)) && p.lamportsOut === 0n && p.tokenOut.length === 0) {
    lines.push(`swap through ${appNames.join(" and ")}`);
  } else if (apps.length) {
    lines.push(`interacts with ${appNames.join(", ")}`);
  }
  for (const d of p.dangers) {
    switch (d.kind) {
      case "set_authority": lines.push(`DANGER: ${d.detail}${d.to ? ` to ${who(d.to)}` : ""}`); break;
      case "approve": lines.push(`DANGER: ${d.detail} to ${d.to ? who(d.to) : "someone"}`); break;
      case "close_account": lines.push(`DANGER: ${d.detail}`); break;
      case "assign": lines.push(`DANGER: ${d.detail}`); break;
      case "lookup_table": lines.push(`note: ${d.detail}`); break;
      case "nonce": lines.push(`note: ${d.detail}`); break;
      case "burn": lines.push(`burns tokens: ${d.detail}`); break;
    }
  }
  if (p.usesDurableNonce) lines.push("note: uses a durable nonce, so it can be broadcast later");
  if (p.memo) lines.push(`memo: "${p.memo}"`);
  if (lines.length === 0) lines.push("does nothing recognisable: no SOL or tokens leave your wallet");

  const spokenParts = lines.filter((l) => !l.startsWith("note:") && !l.startsWith("memo:")).slice(0, 3);
  let spoken = spokenParts.join(". ");
  spoken = spoken.charAt(0).toUpperCase() + spoken.slice(1) + ".";
  if (ctx.origin) spoken = `From ${ctx.origin.replace(/^https?:\/\//, "")}: ${spoken}`;
  return { spoken, lines };
}
