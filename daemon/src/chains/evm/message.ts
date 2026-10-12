/**
 * EVM message signing: EIP-191 personal_sign and EIP-712 typed data. The daemon hashes typed data with viem
 * and sends the controller both the digest and the JSON; the controller re-hashes and compares, so the
 * daemon can never substitute a digest. Permits (EIP-2612, DAI, Permit2) are explained and risk-rated like
 * on-chain approvals: they *are* approvals, just signed off-chain.
 */
import { getAddress, hashMessage, hashTypedData, isAddress, type Hex, type TypedDataDefinition } from "viem";
import type { RiskLevel, RiskVerdict } from "../../risk/filter.js";
import { ROUTER_NAMES } from "../../swap/routers.js";
import type { ChainAddressIntel, ChainTokenMeta } from "../types.js";
import { UNLIMITED_FLOOR } from "./abi.js";
import { fmtEvmToken, shortAddr } from "./explain.js";

export interface TypedData { domain: Record<string, unknown>; types: Record<string, { name: string; type: string }[]>; primaryType: string; message: Record<string, unknown> }

/** Parse the JSON a dapp sends for eth_signTypedData_v4; throws on anything that is not typed data. */
export function parseTypedData(input: string | object): TypedData {
  const j: any = typeof input === "string" ? JSON.parse(input) : input;
  if (!j || typeof j !== "object" || typeof j.primaryType !== "string" || typeof j.types !== "object" || typeof j.message !== "object") throw new Error("not EIP-712 typed data");
  if (!j.types[j.primaryType]) throw new Error(`typed data has no type named ${j.primaryType}`);
  return { domain: j.domain ?? {}, types: j.types, primaryType: j.primaryType, message: j.message };
}

/** Canonical JSON (sorted keys) so the daemon and the controller hash the same bytes regardless of key order. */
export function canonicalTypedDataJson(td: TypedData): string {
  const sort = (v: unknown): unknown => Array.isArray(v) ? v.map(sort) : v && typeof v === "object" ? Object.fromEntries(Object.keys(v as object).sort().map((k) => [k, sort((v as any)[k])])) : v;
  return JSON.stringify(sort({ domain: td.domain, types: td.types, primaryType: td.primaryType, message: td.message }));
}

export function typedDataDigest(td: TypedData): Hex {
  const types = { ...td.types };
  delete (types as any).EIP712Domain; // viem derives the domain type from the domain object
  return hashTypedData({ domain: td.domain, types, primaryType: td.primaryType, message: td.message } as unknown as TypedDataDefinition);
}

export function eip191Digest(message: Uint8Array): Hex { return hashMessage({ raw: message }); }

/** One permit the message grants, normalised across EIP-2612, DAI and Permit2 shapes. */
export interface PermitGrant { token: string | null; spender: string; amount: bigint; unlimited: boolean; deadline: bigint | null; owner: string | null }

const big = (v: unknown): bigint | null => { try { return v == null ? null : BigInt(v as any); } catch { return null; } };
const addr = (v: unknown): string | null => (typeof v === "string" && isAddress(v) ? getAddress(v) : null);

export function permitsOf(td: TypedData): PermitGrant[] {
  const m = td.message as any;
  const vc = addr(td.domain.verifyingContract);
  const out: PermitGrant[] = [];
  switch (td.primaryType) {
    case "Permit": {
      if ("allowed" in m) { // DAI-style
        const allowed = !!m.allowed; if (allowed) out.push({ token: vc, spender: addr(m.spender) ?? "?", amount: UNLIMITED_FLOOR << 1n, unlimited: true, deadline: big(m.expiry), owner: addr(m.holder) });
      } else if ("value" in m) {
        const amount = big(m.value) ?? 0n; out.push({ token: vc, spender: addr(m.spender) ?? "?", amount, unlimited: amount >= UNLIMITED_FLOOR, deadline: big(m.deadline), owner: addr(m.owner) });
      }
      break;
    }
    case "PermitSingle": { const d = m.details ?? {}; const amount = big(d.amount) ?? 0n; out.push({ token: addr(d.token), spender: addr(m.spender) ?? "?", amount, unlimited: amount >= UNLIMITED_FLOOR || amount >= (1n << 160n) - 1n, deadline: big(d.expiration), owner: null }); break; }
    case "PermitBatch": { for (const d of m.details ?? []) { const amount = big(d.amount) ?? 0n; out.push({ token: addr(d.token), spender: addr(m.spender) ?? "?", amount, unlimited: amount >= (1n << 160n) - 1n, deadline: big(d.expiration), owner: null }); } break; }
    case "PermitTransferFrom":
    case "PermitWitnessTransferFrom": { const p = m.permitted ?? {}; const amount = big(p.amount) ?? 0n; out.push({ token: addr(p.token), spender: addr(m.spender) ?? "?", amount, unlimited: amount >= UNLIMITED_FLOOR, deadline: big(m.deadline), owner: null }); break; }
    case "PermitBatchTransferFrom":
    case "PermitBatchWitnessTransferFrom": { for (const p of m.permitted ?? []) { const amount = big(p.amount) ?? 0n; out.push({ token: addr(p.token), spender: addr(m.spender) ?? "?", amount, unlimited: amount >= UNLIMITED_FLOOR, deadline: big(m.deadline), owner: null }); } break; }
  }
  return out;
}

export interface MsgExplainCtx { origin?: string; intel?: Record<string, ChainAddressIntel>; tokenMeta?: Record<string, ChainTokenMeta>; addressBook?: Record<string, string>; nowUnix?: number }

const who = (a: string, ctx: MsgExplainCtx) => {
  const label = ctx.addressBook?.[a] ?? ctx.addressBook?.[a.toLowerCase()] ?? ROUTER_NAMES[a.toLowerCase()];
  if (label) return label;
  const i = ctx.intel?.[a] ?? ctx.intel?.[a.toLowerCase()];
  if (i?.isContract) return `${shortAddr(a)}, a contract${i.ageDays != null && i.ageDays < 30 ? ` deployed ${i.ageDays < 1 ? "today" : Math.floor(i.ageDays) + " days ago"}` : ""}`;
  if (i && !i.isContract && i.exists) return `${shortAddr(a)}, a plain wallet`;
  return shortAddr(a);
};

const fmtDeadline = (d: bigint | null, now: number) => {
  if (d == null) return "";
  if (d >= 1n << 47n) return " forever";
  const days = (Number(d) - now) / 86_400;
  if (days > 36_500) return " forever";
  if (days > 365) return ` for ${Math.round(days / 365)} years`;
  if (days > 1.5) return ` for ${Math.round(days)} days`;
  if (days > 0) return ` for ${Math.max(1, Math.round(days * 24))} hours`;
  return " (already expired)";
};

export function explainTypedData(td: TypedData, ctx: MsgExplainCtx = {}): { spoken: string; lines: string[] } {
  const now = ctx.nowUnix ?? Math.floor(Date.now() / 1000);
  const lines: string[] = [];
  const app = typeof td.domain.name === "string" ? td.domain.name : null;
  const chainId = td.domain.chainId != null ? Number(td.domain.chainId) : undefined;
  const meta = (t: string | null) => (t ? ctx.tokenMeta?.[t] ?? ctx.tokenMeta?.[t.toLowerCase()] : undefined);
  for (const p of permitsOf(td)) {
    const tokenName = p.token ? fmtEvmToken(null, p.token, meta(p.token), chainId) : "tokens";
    if (p.unlimited) lines.push(`DANGER: gives ${who(p.spender, ctx)} unlimited permission to spend your ${tokenName}${fmtDeadline(p.deadline, now)}`);
    else lines.push(`lets ${who(p.spender, ctx)} spend up to ${fmtEvmToken(p.amount, p.token, meta(p.token), chainId)}${fmtDeadline(p.deadline, now)}`);
  }
  if (lines.length === 0) lines.push(`signs a "${td.primaryType}" message${app ? ` for ${app}` : ""}`);
  if (td.domain.chainId != null) lines.push(`note: on chain ${td.domain.chainId}`);
  const spokenParts = lines.filter((l) => !l.startsWith("note:")).slice(0, 3);
  let spoken = spokenParts.join(". ");
  spoken = spoken.charAt(0).toUpperCase() + spoken.slice(1) + ".";
  if (ctx.origin) spoken = `From ${ctx.origin.replace(/^https?:\/\//, "")}: ${spoken}`;
  return { spoken, lines };
}

export function assessTypedData(td: TypedData, ctx: { intel?: Record<string, ChainAddressIntel>; routers?: Set<string>; nowUnix?: number; requestedChainId?: number } = {}): RiskVerdict {
  const now = ctx.nowUnix ?? Math.floor(Date.now() / 1000);
  const reasons: string[] = [];
  let level: RiskLevel = "low";
  const rank: Record<RiskLevel, number> = { low: 0, medium: 1, high: 2, block: 3 };
  const raise = (to: RiskLevel, why: string) => { reasons.push(why); if (rank[to] > rank[level]) level = to; };
  const permits = permitsOf(td);
  for (const p of permits) {
    const i = ctx.intel?.[p.spender] ?? ctx.intel?.[p.spender.toLowerCase()];
    const isRouter = ctx.routers?.has(p.spender.toLowerCase());
    if (i && i.exists && !i.isContract) raise("block", `it lets ${shortAddr(p.spender)}, a plain wallet, spend your tokens. Only contracts need permits.`);
    if (p.unlimited) raise("high", `it gives ${isRouter ? "a router" : shortAddr(p.spender)} unlimited permission to spend your tokens.`);
    if (p.deadline != null && (p.deadline >= 1n << 47n || Number(p.deadline) - now > 30 * 86_400)) raise("high", "the permission lasts far into the future; a permit should expire within minutes.");
    if (p.deadline != null && Number(p.deadline) < now) raise("medium", "the permit is already expired, so it does nothing.");
    if (i && i.isContract && i.ageDays != null && i.ageDays < 3 && !isRouter) raise("high", `the spender is a contract deployed ${i.ageDays < 1 ? "today" : Math.floor(i.ageDays) + " days ago"}.`);
    if (!p.unlimited && isRouter) raise("low", "a limited permit to a known router.");
  }
  if (ctx.requestedChainId != null && td.domain.chainId != null && Number(td.domain.chainId) !== ctx.requestedChainId) raise("block", `the message is for chain ${td.domain.chainId} but the site is on chain ${ctx.requestedChainId}.`);
  if (permits.length === 0 && typeof td.message === "object" && JSON.stringify(td.message).length > 4000) raise("medium", "it is a very long structured message I cannot summarise.");
  if (reasons.length === 0) reasons.push("nothing unusual.");
  return { level, reasons };
}

/** EIP-191 text: refuse disguised transactions and warn on blind hashes. */
export function assessPersonalMessage(message: Uint8Array, looksLikeTx: boolean): RiskVerdict {
  if (looksLikeTx) return { level: "block", reasons: ["the \"message\" is really a transaction. Not signing it blind."] };
  const text = new TextDecoder().decode(message);
  if (/^0x[0-9a-fA-F]{64}$/.test(text.trim()) || message.length === 32) return { level: "high", reasons: ["it asks me to sign a raw hash I cannot read. That is how approvals get smuggled."] };
  return { level: "low", reasons: ["nothing unusual."] };
}
