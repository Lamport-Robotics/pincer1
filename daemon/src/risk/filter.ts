/**
 * The scam filter. Input: a parsed transaction plus context. Output: a level and the reasons, in the
 * order they should be spoken.
 *
 *   block  → the daemon refuses without asking the owner. The robot says why.
 *   high   → the owner is asked, and the robot leads with the warning.
 *   medium → the owner is asked with a caveat.
 *   low    → the owner is asked normally.
 *
 * Rules are deliberately boring and explicit; each one is a sentence the robot can say.
 */
import { TRUSTED_PROGRAMS, U64_MAX } from "../tx/known.js";
import type { ParsedTx } from "../tx/parse.js";
import { short } from "../tx/explain.js";
import type { AddressIntel } from "./intel.js";

export type RiskLevel = "low" | "medium" | "high" | "block";

export interface RiskInput {
  tx: ParsedTx;
  owner: string;
  /** Owner's SOL balance in lamports, if known. */
  ownerLamports?: number;
  intel?: Record<string, AddressIntel>;
  addressBook?: Record<string, string>; // pubkey → label
  origin?: string;
  originAllowlist?: string[];
  dailyLimitLamports?: bigint;
  /** Programs this owner has interacted with before (from the daemon's history). */
  seenPrograms?: Set<string>;
  nowUnix?: number;
}

export interface RiskVerdict {
  level: RiskLevel;
  reasons: string[];
}

const rank: Record<RiskLevel, number> = { low: 0, medium: 1, high: 2, block: 3 };

/** Two addresses that share a prefix and a suffix but differ in between: the classic poisoning trick. */
export function isLookalike(candidate: string, known: string): boolean {
  if (candidate === known) return false;
  if (candidate.length < 32 || known.length < 32) return false;
  return candidate.slice(0, 4) === known.slice(0, 4) && candidate.slice(-4) === known.slice(-4);
}

export function assess(input: RiskInput): RiskVerdict {
  const { tx, owner } = input;
  const reasons: string[] = [];
  let level: RiskLevel = "low";
  const raise = (to: RiskLevel, why: string) => {
    reasons.push(why);
    if (rank[to] > rank[level]) level = to;
  };
  const book = input.addressBook ?? {};
  const bookKeys = Object.keys(book);
  const intelOf = (a: string) => input.intel?.[a];

  // ── hard blocks: the transaction itself is a weapon ──
  for (const d of tx.dangers) {
    if (d.kind === "set_authority") raise("block", `it hands over control of your token account${d.to ? ` to ${short(d.to)}` : ""}. That's how drainers work.`);
    if (d.kind === "assign") raise("block", `it reassigns your wallet account to another program. Nothing legitimate does that.`);
    if (d.kind === "close_account" && d.to && d.to !== owner && !book[d.to]) raise("block", `it closes your token account and sends the rent to a stranger.`);
    if (d.kind === "approve") {
      const toIntel = d.to ? intelOf(d.to) : undefined;
      if (d.amount === U64_MAX && d.to && !toIntel?.isProgram) raise("block", `it gives ${short(d.to)}, a plain wallet, unlimited permission to spend your tokens.`);
      else if (d.amount === U64_MAX) raise("high", `it gives a program unlimited permission to spend your tokens.`);
      else raise("medium", `it delegates some of your tokens to ${d.to ? short(d.to) : "another account"}.`);
    }
    if (d.kind === "lookup_table") raise("medium", "it uses address lookup tables I could not fully read, so part of it is opaque.");
    if (d.kind === "burn") raise("medium", "it burns some of your tokens.");
  }

  // ── destination checks ──
  if (tx.dest && tx.lamportsOut > 0n) {
    const dest = tx.dest;
    const i = intelOf(dest);
    const inBook = !!book[dest];
    for (const k of bookKeys) {
      if (isLookalike(dest, k)) raise("block", `the destination looks like your contact "${book[k]}" but is not the same address. This is address poisoning.`);
    }
    if (!inBook && i) {
      if (!i.exists) raise("high", "the destination has never been seen on chain.");
      else if (i.ageDays != null && i.ageDays < 3) raise("high", `the destination wallet is ${i.ageDays < 1 ? "less than a day" : Math.floor(i.ageDays) + " days"} old.`);
      else if (i.txCount < 3) raise("high", `the destination wallet has almost no history.`);
    }
    if (input.ownerLamports != null && input.ownerLamports > 0) {
      const share = Number(tx.lamportsOut) / input.ownerLamports;
      if (share > 0.9 && !inBook) raise("block", `it empties your wallet: ${Math.round(share * 100)}% of your SOL to an address not in your contacts.`);
      else if (share > 0.5 && !inBook) raise("high", `it moves ${Math.round(share * 100)}% of your SOL.`);
    }
    if (input.dailyLimitLamports != null && tx.lamportsOut > input.dailyLimitLamports) raise("medium", "it is above your daily limit; I'll need a long touch.");
  }

  // ── token transfers to strangers ──
  for (const t of tx.tokenOut) {
    const i = intelOf(t.dest);
    if (i && i.ageDays != null && i.ageDays < 3 && !book[t.dest]) raise("high", `tokens go to a wallet created ${i.ageDays < 1 ? "today" : Math.floor(i.ageDays) + " days ago"}.`);
  }

  // ── programs ──
  for (const p of tx.programs) {
    if (TRUSTED_PROGRAMS.has(p)) continue;
    const i = intelOf(p);
    const seen = input.seenPrograms?.has(p);
    if (i && !i.isProgram) raise("block", `${short(p)} is invoked as a program but is not executable. The transaction is malformed or hostile.`);
    else if (i && i.ageDays != null && i.ageDays < 7) raise("high", `it calls a program deployed ${Math.floor(i.ageDays)} days ago that I don't know.`);
    else if (!seen) raise("medium", `it calls ${short(p)}, a program you haven't used before.`);
  }

  // ── origin ──
  if (input.origin && input.originAllowlist && !input.originAllowlist.some((o) => input.origin!.startsWith(o))) {
    if (tx.lamportsOut > 1_000_000_000n || tx.tokenOut.length) raise("medium", `the request comes from ${input.origin.replace(/^https?:\/\//, "")}, which is not on your trusted sites.`);
  }

  // ── durable nonce from a dapp is a classic "sign now, drain later" ──
  if (tx.usesDurableNonce && input.origin) raise("high", "it uses a durable nonce, so whoever holds it can broadcast it any time later.");

  if (reasons.length === 0) reasons.push("nothing unusual.");
  return { level, reasons };
}
