import { originAllowed } from "../../risk/filter.js";
/**
 * The EVM scam filter. Same shape as risk/filter.ts: explicit rules, each one a sentence the robot can
 * say, the worst level wins.
 *
 *   block  → refused without asking.      high → asked, warning first.
 *   medium → asked with a caveat.          low → asked normally.
 */
import { formatUnits } from "viem";
import type { RiskLevel, RiskVerdict } from "../../risk/filter.js";
import { fmtNativeEvm, shortAddr } from "./explain.js";
import type { AssessCtx } from "../types.js";
import type { EvmParsed } from "./parse.js";

const rank: Record<RiskLevel, number> = { low: 0, medium: 1, high: 2, block: 3 };

/** Same prefix+suffix, different middle: address poisoning. EVM prefix check skips the 0x. */
export function isEvmLookalike(candidate: string, known: string): boolean {
  const c = candidate.toLowerCase(), k = known.toLowerCase();
  if (c === k || c.length !== 42 || k.length !== 42) return false;
  return c.slice(0, 6) === k.slice(0, 6) && c.slice(-4) === k.slice(-4);
}

export function assessEvm(p: EvmParsed, ctx: AssessCtx): RiskVerdict {
  const reasons: string[] = [];
  let level: RiskLevel = "low";
  const raise = (to: RiskLevel, why: string) => { reasons.push(why); if (rank[to] > rank[level]) level = to; };
  const book = (a: string) => ctx.addressBook[a] ?? ctx.addressBook[a.toLowerCase()];
  const intelOf = (a: string) => ctx.intel[a] ?? ctx.intel[a.toLowerCase()];
  const isRouter = (a: string) => ctx.routers.has(a.toLowerCase());
  const c = p.call;

  // ── chain id: the dapp said one chain, the bytes say another ──
  if (p.requestedChainId != null && p.requestedChainId !== p.chainId) raise("block", `the site asked for chain ${p.requestedChainId} but the transaction is for chain ${p.chainId}. Something is lying.`);

  // ── hard blocks: the transaction itself is a weapon ──
  if (c.fn === "setApprovalForAll" && c.approved) raise("block", `it hands ${shortAddr(c.operator)} control over every item in that collection. That's how NFT drainers work.`);
  for (const d of p.dangers) {
    if (d.kind !== "approve_unlimited" && d.kind !== "approve") continue;
    const spender = d.to!;
    const si = intelOf(spender);
    if (si && si.exists && !si.isContract) { raise("block", `it gives ${shortAddr(spender)}, a plain wallet, permission to spend your tokens. Only contracts need that.`); continue; }
    if (d.kind === "approve_unlimited") {
      if (isRouter(spender) && ctx.unlimitedApproveToRouters) raise("medium", `it gives a known router unlimited permission to spend your tokens; you allowed that for routers.`);
      else if (isRouter(spender)) raise("block", `it asks for unlimited permission to spend your tokens. I only sign exact-amount approvals, even for known routers.`);
      else raise("block", `it gives ${shortAddr(spender)} unlimited permission to spend your tokens. That's how drainers work.`);
    } else {
      if (isRouter(spender)) raise("low", "an exact-amount approval to a known router.");
      else if (si?.isContract && si.ageDays != null && si.ageDays < 3) raise("high", `it lets a contract deployed ${si.ageDays < 1 ? "today" : Math.floor(si.ageDays) + " days ago"} spend some of your tokens.`);
      else raise("medium", `it lets ${shortAddr(spender)} spend some of your tokens.`);
    }
  }
  if (c.fn === "create") raise("high", "it deploys a contract from your wallet. Wallets don't normally do that.");

  // ── the contract being called ──
  if (p.to && p.data !== "0x") {
    const ti = intelOf(p.to);
    if (ti && ti.exists && !ti.isContract) raise("high", `it sends function data to ${shortAddr(p.to)}, a plain wallet that cannot run it.`);
    if (ti && !ti.exists) raise("block", `it calls ${shortAddr(p.to)}, which has no code and no history. Nothing is there.`);
    if (c.fn === "unknown" && c.selector) {
      if (!isRouter(p.to) && ti?.isContract && ti.ageDays == null && ti.verified !== true) raise("high", `it calls ${shortAddr(p.to)}, a contract I know nothing about: no verified source, no known age.`);
      else if (!isRouter(p.to) && ti?.isContract && ti.ageDays != null && ti.ageDays < 7) raise("high", `it calls a contract deployed ${Math.floor(ti.ageDays)} days ago that I don't know.`);
      else if (!isRouter(p.to) && !ctx.knownPrograms.has(p.to.toLowerCase())) raise(p.value > 0n ? "high" : "medium", `it calls ${shortAddr(p.to)}, a contract you haven't approved${p.value > 0n ? ", and value leaves your wallet" : ""}.`);
    }
  }

  // ── native value destination ──
  if (p.value > 0n && p.to) {
    const dest = p.to;
    const i = intelOf(dest);
    const inBook = !!book(dest);
    for (const k of Object.keys(ctx.addressBook)) if (isEvmLookalike(dest, k)) raise("block", `the destination looks like your contact "${ctx.addressBook[k]}" but is not the same address. This is address poisoning.`);
    if (!inBook && i && !isRouter(dest)) {
      if (!i.exists) raise("high", "the destination has never been seen on chain.");
      else if (i.isContract && i.ageDays != null && i.ageDays < 3) raise("high", `the destination is a contract deployed ${i.ageDays < 1 ? "less than a day" : Math.floor(i.ageDays) + " days"} ago.`);
      else if (!i.isContract && i.ageDays != null && i.ageDays < 3) raise("high", `the destination wallet is ${i.ageDays < 1 ? "less than a day" : Math.floor(i.ageDays) + " days"} old.`);
      else if (!i.isContract && i.txCount < 3 && (i.balance ?? 0n) === 0n) raise("high", "the destination wallet has almost no history.");
    }
    if (ctx.ownerBalance != null && ctx.ownerBalance > 0n) {
      const share = Number(p.value) / Number(ctx.ownerBalance);
      if (share > 0.9 && !inBook) raise("block", `it empties your wallet: ${Math.round(share * 100)}% of your ${fmtNativeEvm(0n, p.chainId).split(" ")[1]} to an address not in your contacts.`);
      else if (share > 0.5 && !inBook) raise("high", `it moves ${Math.round(share * 100)}% of your balance.`);
    }
    if (p.value > ctx.dailyLimit) raise("medium", "it is above your daily limit; I'll need a long touch.");
    // gas sanity: paying more than a tenth of the value in fees is a sign of a mis-set or hostile gas limit
    if (p.maxFeeWei * 10n > p.value) raise("medium", `the fee can reach ${Number(formatUnits(p.maxFeeWei * 10_000n / p.value, 2)).toFixed(0)}% of the amount sent.`);
  }
  // fee without value: still cap absurd gas (> 0.05 native)
  if (p.value === 0n && p.maxFeeWei > 5n * 10n ** 16n) raise("medium", `the fee can reach ${fmtNativeEvm(p.maxFeeWei, p.chainId)}, which is a lot for a call.`);

  // ── token transfers to strangers ──
  for (const t of p.tokenOut) {
    const i = intelOf(t.dest);
    if (i && !i.isContract && i.ageDays != null && i.ageDays < 3 && !book(t.dest)) raise("high", `tokens go to a wallet created ${i.ageDays < 1 ? "today" : Math.floor(i.ageDays) + " days ago"}.`);
    if (i && !i.exists && !book(t.dest)) raise("high", "tokens go to an address never seen on chain.");
    for (const k of Object.keys(ctx.addressBook)) if (isEvmLookalike(t.dest, k)) raise("block", `the token recipient looks like your contact "${ctx.addressBook[k]}" but is not the same address. This is address poisoning.`);
  }

  // ── origin ──
  if (ctx.origin && ctx.originAllowlist.length && !originAllowed(ctx.origin, ctx.originAllowlist)) {
    if (p.value > 5n * 10n ** 17n || p.tokenOut.length || p.dangers.length) raise("medium", `the request comes from ${ctx.origin.replace(/^https?:\/\//, "")}, which is not on your trusted sites.`);
  }

  if (reasons.length === 0) reasons.push("nothing unusual.");
  return { level, reasons };
}
