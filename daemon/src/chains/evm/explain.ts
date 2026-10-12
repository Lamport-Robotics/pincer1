/**
 * Sentences the robot says for an EVM transaction. Same voice as tx/explain.ts: short, concrete, no hex
 * beyond a shortened address, one danger per sentence.
 */
import { formatUnits } from "viem";
import type { Explanation } from "../../tx/explain.js";
import type { ChainTokenMeta, ExplainCtx } from "../types.js";
import { KNOWN_TOKENS } from "./abi.js";
import type { EvmParsed } from "./parse.js";
import { CHAIN_NAMES, NATIVE_SYMBOL } from "./rpc.js";
import { ROUTER_NAMES } from "../../swap/routers.js";

/** "0x1234…abcd": the 0x, four leading and four trailing characters. */
export const shortAddr = (a: string) => (a.length > 12 ? `${a.slice(0, 6)}…${a.slice(-4)}` : a);

export function fmtNativeEvm(wei: bigint, chainId = 1): string {
  const sym = NATIVE_SYMBOL[chainId] ?? "ETH";
  if (wei === 0n) return `0 ${sym}`;
  const v = Number(formatUnits(wei, 18));
  if (v < 0.000001) return `${wei} wei`;
  return `${v.toLocaleString("en-US", { maximumFractionDigits: v < 1 ? 6 : 4 })} ${sym}`;
}

/** amount null → just the token's name. */
export function fmtEvmToken(amount: bigint | null, token: string | null, meta?: ChainTokenMeta, chainId?: number): string {
  const known = token && chainId != null ? KNOWN_TOKENS[chainId]?.[token.toLowerCase()] : undefined;
  const decimals = meta?.decimals ?? known?.decimals;
  const symbol = meta?.symbol ?? known?.symbol ?? (token ? `token ${shortAddr(token)}` : "tokens");
  if (amount == null) return symbol;
  if (decimals == null) return `${amount.toString()} units of ${symbol}`;
  const v = Number(formatUnits(amount, decimals));
  return `${v.toLocaleString("en-US", { maximumFractionDigits: v < 1 ? 6 : 2 })} ${symbol}`;
}

const metaOf = (ctx: ExplainCtx, token: string) => ctx.tokenMeta[token] ?? ctx.tokenMeta[token.toLowerCase()];

export function explainEvm(p: EvmParsed, ctx: ExplainCtx): Explanation {
  const lines: string[] = [];
  const book = (a: string) => ctx.addressBook[a] ?? ctx.addressBook[a.toLowerCase()];
  const intelOf = (a: string) => ctx.intel[a] ?? ctx.intel[a.toLowerCase()];
  const who = (a: string) => {
    const label = book(a);
    if (label) return label;
    const router = ROUTER_NAMES[a.toLowerCase()];
    if (router) return router;
    const i = intelOf(a);
    if (i?.isContract) {
      const age = i.ageDays == null ? "" : i.ageDays < 1 ? " deployed today" : i.ageDays < 30 ? ` deployed ${Math.floor(i.ageDays)} days ago` : "";
      const ver = i.verified === true ? "a verified contract" : i.verified === false ? "an unverified contract" : "a contract";
      return `${shortAddr(a)}, ${ver}${age}`;
    }
    if (i && i.exists) {
      const age = i.ageDays == null ? "" : i.ageDays < 1 ? " created today" : i.ageDays < 30 ? ` ${Math.floor(i.ageDays)} days old` : ` ${Math.floor(i.ageDays / 30)} months old`;
      return `${shortAddr(a)}, a wallet${age} with ${i.txCount}${i.txCountIsLowerBound ? "+" : ""} transactions`;
    }
    if (i && !i.exists) return `${shortAddr(a)}, an address never seen on chain`;
    return shortAddr(a);
  };
  const tok = (amount: bigint | null, token: string) => fmtEvmToken(amount, token, metaOf(ctx, token), p.chainId);
  const c = p.call;

  if (p.value > 0n && p.to) lines.push(`sends ${fmtNativeEvm(p.value, p.chainId)} to ${who(p.to)}`);
  switch (c.fn) {
    case "transfer": lines.push(`sends ${tok(c.amount, c.token)} to ${who(c.to)}`); break;
    case "transferFrom": lines.push(`moves ${tok(c.amount, c.token)} from ${shortAddr(c.from)} to ${who(c.to)}`); break;
    case "permit2TransferFrom": lines.push(`moves ${tok(c.amount, c.token)} through Permit2 to ${who(c.to)}`); break;
    case "approve":
    case "increaseAllowance":
      if (c.amount === 0n) lines.push(`revokes ${who(c.spender)}'s permission to spend your ${tok(null, c.token)}`);
      else if (c.unlimited) lines.push(`DANGER: gives ${who(c.spender)} unlimited permission to spend your ${tok(null, c.token)}`);
      else lines.push(`lets ${who(c.spender)} spend up to ${tok(c.amount, c.token)}`);
      break;
    case "decreaseAllowance": lines.push(`reduces ${who(c.spender)}'s allowance on your ${tok(null, c.token)}`); break;
    case "permit2Approve":
      if (c.unlimited) lines.push(`DANGER: gives ${who(c.spender)} unlimited permission to spend your ${tok(null, c.token)} through Permit2`);
      else lines.push(`lets ${who(c.spender)} spend up to ${tok(c.amount, c.token)} through Permit2`);
      break;
    case "permit":
      if (c.unlimited) lines.push(`DANGER: submits a permit giving ${who(c.spender)} unlimited permission to spend your ${tok(null, c.token)}`);
      else lines.push(`submits a permit letting ${who(c.spender)} spend up to ${tok(c.amount, c.token)}`);
      break;
    case "setApprovalForAll":
      if (c.approved) lines.push(`DANGER: gives ${who(c.operator)} control over every item you own in collection ${shortAddr(c.collection)}`);
      else lines.push(`revokes ${who(c.operator)}'s control over collection ${shortAddr(c.collection)}`);
      break;
    case "router": lines.push(`swap through ${who(c.router)}`); break;
    case "create": lines.push("DANGER: deploys a new contract from your wallet"); break;
    case "unknown":
      if (c.selector && p.to) lines.push(`calls a function I don't recognise on ${who(p.to)}`);
      break;
  }
  if (lines.length === 0) lines.push("does nothing recognisable: no value or tokens leave your wallet");
  lines.push(`note: fee up to ${fmtNativeEvm(p.maxFeeWei, p.chainId)}`);
  if (p.type === "legacy") lines.push("note: legacy transaction format");

  const spokenParts = lines.filter((l) => !l.startsWith("note:")).slice(0, 3);
  let spoken = spokenParts.join(". ");
  spoken = spoken.charAt(0).toUpperCase() + spoken.slice(1) + ".";
  const chain = CHAIN_NAMES[p.chainId] ?? `chain ${p.chainId}`;
  spoken = ctx.origin ? `From ${ctx.origin.replace(/^https?:\/\//, "")}, on ${chain}: ${spoken}` : `On ${chain}: ${spoken}`;
  return { spoken, lines };
}
