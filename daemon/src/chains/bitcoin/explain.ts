/** Spoken sentences and the scam filter for Bitcoin PSBTs. */
import type { Explanation } from "../../tx/explain.js";
import { short } from "../../tx/explain.js";
import type { RiskLevel, RiskVerdict } from "../../risk/filter.js";
import type { AssessCtx, ExplainCtx } from "../types.js";
import { DUST_INPUT_SATS, type BtcParsed } from "./psbt.js";

export function fmtBtc(sats: bigint): string {
  if (sats === 0n) return "0 BTC";
  if (sats < 100_000n) return `${Number(sats).toLocaleString("en-US")} sats`;
  const v = Number(sats) / 1e8;
  return `${v.toLocaleString("en-US", { maximumFractionDigits: v < 1 ? 6 : 4 })} BTC`;
}
export const fmtSats = (sats: bigint) => `${Number(sats).toLocaleString("en-US")} sats`;
const shortBtc = (a: string) => (a.length > 14 ? `${a.slice(0, 8)}…${a.slice(-4)}` : a);

export function explainBtc(p: BtcParsed, ctx: ExplainCtx): Explanation {
  const lines: string[] = [];
  const who = (a: string) => {
    const label = ctx.addressBook[a];
    if (label) return label;
    const i = ctx.intel[a];
    if (i && i.txCount > 0) return `${shortBtc(a)}, an address used ${i.txCount}${i.txCountIsLowerBound ? "+" : ""} times before`;
    if (i && !i.exists) return `${shortBtc(a)}, a fresh address`;
    return shortBtc(a);
  };
  const pct = p.satsOut > 0n && p.feeSats >= 0n ? ` (${(Number(p.feeSats) / Number(p.satsOut) * 100).toLocaleString("en-US", { maximumFractionDigits: 1 })}%)` : "";
  if (p.dest && p.satsOut > 0n) lines.push(`sends ${fmtBtc(p.satsOut)} to ${who(p.dest)}, fee ${p.feeSats >= 0n ? fmtSats(p.feeSats) + pct : "unknown"}`);
  else if (p.satsOut === 0n && p.ownerInputs.length) lines.push(`moves ${fmtBtc(BigInt(p.ownerInputs.reduce((a, i) => a + i.value, 0)))} back to yourself, fee ${p.feeSats >= 0n ? fmtSats(p.feeSats) : "unknown"}`);
  for (const d of p.dangers) {
    if (d.kind === "sighash_not_all") lines.push(`DANGER: ${d.detail}`);
    else if (d.kind === "unknown_change") lines.push(`DANGER: ${d.detail}`);
    else if (d.kind === "high_fee") lines.push(`warning: ${d.detail}`);
  }
  if (p.memo) lines.push(`memo: "${p.memo}"`);
  if (p.rbf) lines.push("note: can be replaced with a higher fee until it confirms");
  if (p.foreignInputs) lines.push(`note: ${p.foreignInputs} input${p.foreignInputs > 1 ? "s" : ""} belong to someone else`);
  lines.push(`note: spends ${p.ownerInputs.length} coin${p.ownerInputs.length === 1 ? "" : "s"}${p.changeSats > 0n ? `, ${fmtBtc(p.changeSats)} comes back as change` : ""}`);
  if (lines.filter((l) => !l.startsWith("note:")).length === 0) lines.unshift("does nothing recognisable: no coins leave your wallet");
  const spokenParts = lines.filter((l) => !l.startsWith("note:") && !l.startsWith("memo:")).slice(0, 3);
  let spoken = spokenParts.join(". ");
  spoken = spoken.charAt(0).toUpperCase() + spoken.slice(1) + ".";
  if (ctx.origin) spoken = `From ${ctx.origin.replace(/^https?:\/\//, "")}: ${spoken}`;
  return { spoken, lines };
}

const rank: Record<RiskLevel, number> = { low: 0, medium: 1, high: 2, block: 3 };

export function assessBtc(p: BtcParsed, ctx: AssessCtx): RiskVerdict {
  const reasons: string[] = [];
  let level: RiskLevel = "low";
  const raise = (to: RiskLevel, why: string) => { reasons.push(why); if (rank[to] > rank[level]) level = to; };

  if (p.ownerInputs.length === 0) raise("block", "none of the coins being spent are yours, so there is nothing for me to sign.");
  if (p.inputsWithoutValue > 0) raise("block", "the request hides the value of some coins, so I cannot tell what the fee is.");
  if (!p.sighashAll) raise("block", "it does not lock the outputs (sighash is not ALL), so whoever holds it could redirect the coins after I sign.");
  for (const d of p.dangers) if (d.kind === "unknown_change" && !d.detail.startsWith("burns")) raise("block", `${d.detail}.`);
  for (const d of p.dangers) if (d.kind === "unknown_change" && d.detail.startsWith("burns")) raise("block", "it burns coins into a data output.");
  if (p.feeSats < 0n && p.ownerInputs.length && p.inputsWithoutValue === 0 && p.foreignInputs === 0) raise("block", "the outputs are worth more than the inputs; the transaction is malformed.");

  if (p.feeSats >= 0n && p.satsOut > 0n) {
    const share = Number(p.feeSats) / Number(p.satsOut);
    if (share > 0.05) raise("high", `the fee is ${Math.round(share * 100)}% of the amount sent. That's far too much.`);
    else if (share > 0.02) raise("medium", `the fee is ${(share * 100).toFixed(1)}% of the amount sent.`);
  }
  if (p.feeSats > 1_000_000n) raise("high", `the fee alone is ${fmtBtc(p.feeSats)}.`);

  if (p.dest) {
    const i = ctx.intel[p.dest];
    const inBook = !!ctx.addressBook[p.dest];
    if (!inBook && i && i.txCount >= 20) raise("medium", `the destination address has been used ${i.txCount}${i.txCountIsLowerBound ? "+" : ""} times; reused addresses are a dusting and tracking pattern.`);
    for (const k of Object.keys(ctx.addressBook)) {
      if (k !== p.dest && k.length === p.dest.length && k.slice(0, 8) === p.dest.slice(0, 8) && k.slice(-4) === p.dest.slice(-4)) raise("block", `the destination looks like your contact "${ctx.addressBook[k]}" but is not the same address. This is address poisoning.`);
    }
  }
  const dustInputs = p.ownerInputs.filter((i) => i.value < DUST_INPUT_SATS).length;
  if (dustInputs) raise("medium", `it spends ${dustInputs} dust-sized coin${dustInputs > 1 ? "s" : ""} someone may have sent you to track your wallet.`);

  if (ctx.ownerBalance != null && ctx.ownerBalance > 0n && p.satsOut > 0n) {
    const share = Number(p.satsOut) / Number(ctx.ownerBalance);
    if (share > 0.9 && !(p.dest && ctx.addressBook[p.dest])) raise("block", `it empties your wallet: ${Math.round(share * 100)}% of your bitcoin to an address not in your contacts.`);
    else if (share > 0.5 && !(p.dest && ctx.addressBook[p.dest])) raise("high", `it moves ${Math.round(share * 100)}% of your bitcoin.`);
  }
  if (p.satsOut > ctx.dailyLimit) raise("medium", "it is above your daily limit; I'll need a long touch.");
  if (ctx.origin && ctx.originAllowlist.length && !ctx.originAllowlist.some((o) => ctx.origin!.startsWith(o)) && p.satsOut > 100_000n) raise("medium", `the request comes from ${ctx.origin.replace(/^https?:\/\//, "")}, which is not on your trusted sites.`);
  if (p.foreignInputs > 0) raise("medium", "other people's coins are in this transaction (a coinjoin or payjoin).");

  if (reasons.length === 0) reasons.push("nothing unusual.");
  return { level, reasons };
}

export { short };
