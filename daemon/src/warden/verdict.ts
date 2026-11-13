/**
 * Warden: what a signature request will do, in one sentence, and how much friction it deserves.
 *
 * Rules decide; the sentence explains. Nothing here asks a model for anything: the decoder (tx/parse), the
 * scam filter (risk/filter) and the explainer (tx/explain) are deterministic, and this file adds the owner's
 * own history on top of them.
 *
 * Four levels, so that most requests stay silent and a warning still means something:
 *   allow    on pattern: an address and a size the owner has confirmed before. Say nothing.
 *   explain  show the sentence; an ordinary approval is enough.
 *   confirm  the owner must retype something that proves they read it (the address's last four characters,
 *            or an acknowledgement when there is nothing to retype).
 *   block    not passed on at all.
 *
 * Untrusted text (memos, site names) only ever appears quoted inside the sentence. It never reaches a rule.
 */
import { parseAnyMessage, checkSignMessage, isRingKeyDerivation, touchesPrivacyRings, type ParsedTx, type ParseOptions } from "../tx/parse.js";
import { explain, short, fmtSol } from "../tx/explain.js";
import { assess, originAllowed, type RiskLevel } from "../risk/filter.js";
import type { AddressIntel } from "../risk/intel.js";
import type { OwnerMemory } from "./memory.js";

export type WardenLevel = "allow" | "explain" | "confirm" | "block";

export interface WardenVerdict {
  level: WardenLevel;
  /** One or two plain sentences, built from decoded facts only. */
  sentence: string;
  /** Why the level is what it is, most serious first. Empty for `allow`. */
  reasons: string[];
  /** What the owner must retype for `confirm`. */
  confirm?: { kind: "last4" | "acknowledge"; expect: string };
  /** The decoded transaction, when there is one (for the caller's own display and for recording a send). */
  parsed?: ParsedTx;
}

export interface TransactionInput {
  /** The serialized Solana message to be signed. */
  bytes: Uint8Array;
  owner: string;
  memory: OwnerMemory;
  origin?: string;
  /** Owner's SOL balance in lamports, if known. */
  ownerLamports?: number;
  intel?: Record<string, AddressIntel>;
  addressBook?: Record<string, string>;
  knownPrograms?: Set<string>;
  originAllowlist?: string[];
  lookupTables?: ParseOptions["lookupTables"];
  ownerTokenAccounts?: Set<string>;
}

export interface MessageInput {
  bytes: Uint8Array;
  origin?: string;
  /** The wallet's public key, when known (lets the off-chain signer and sign-in checks run). */
  ownKey?: Uint8Array | null;
  /** Sites the owner allowed to ask for the Privacy Rings key signature. Exact origin match. */
  ringSites?: string[];
}

/** A send is "large for this owner" past three times the largest they have confirmed, once there is history. */
const UNUSUAL_FACTOR = 3n;
const MIN_HISTORY = 5;

const site = (origin?: string) => (origin ? origin.replace(/^https?:\/\//, "").replace(/\/.*$/, "") : "A site");
const last4 = (a: string) => a.slice(-4);
const capital = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);

function fromRisk(level: RiskLevel): WardenLevel {
  return level === "block" ? "block" : level === "high" ? "confirm" : level === "medium" ? "explain" : "allow";
}
const order: Record<WardenLevel, number> = { allow: 0, explain: 1, confirm: 2, block: 3 };
const worse = (a: WardenLevel, b: WardenLevel): WardenLevel => (order[a] >= order[b] ? a : b);

/** Every address that receives value from the owner in this transaction. */
export function destinationsOf(p: ParsedTx): string[] {
  const out: string[] = [];
  if (p.lamportsOut > 0n && p.dest) out.push(p.dest);
  for (const t of p.tokenOut) if (t.dest) out.push(t.dest);
  return [...new Set(out)];
}

export function checkTransaction(input: TransactionInput): WardenVerdict {
  const { bytes, owner, memory } = input;

  // Legacy, v0 and version 1 are all read. What does not decode cleanly is never guessed at.
  let parsed: ParsedTx;
  try {
    parsed = parseAnyMessage(bytes, { owner, lookupTables: input.lookupTables, ownerTokenAccounts: input.ownerTokenAccounts });
  } catch {
    return {
      level: "block",
      sentence: "This is not a transaction I can read, so I can't tell you what it does.",
      reasons: ["it does not decode as a Solana transaction."],
    };
  }
  // Bytes that decode to nothing, or to a transaction this wallet does not sign, are not something to wave
  // through: the caller handed over the wrong thing (a whole signed transaction, say) or the request is malformed.
  if (parsed.instructions.length === 0 || !parsed.signers.includes(owner)) {
    return {
      level: "block",
      sentence: parsed.instructions.length === 0 ? "This request decodes to a transaction with nothing in it, so I can't tell you what it does." : "This transaction is not one your wallet signs, so there is nothing for you to approve.",
      reasons: [parsed.instructions.length === 0 ? "it decodes to no instructions." : "the wallet is not a signer of it."],
    };
  }
  if (touchesPrivacyRings(bytes)) {
    return {
      level: "block",
      sentence: "This calls the Privacy Rings pool or registry. I can't check a private amount yet, so I won't pass it on.",
      reasons: ["it calls the Privacy Rings programs, which are not decoded yet."],
      parsed,
    };
  }

  const book = input.addressBook ?? {};
  const risk = assess({
    tx: parsed, owner, ownerLamports: input.ownerLamports, intel: input.intel, addressBook: book,
    origin: input.origin, originAllowlist: input.originAllowlist, knownPrograms: input.knownPrograms,
  });
  let level = fromRisk(risk.level);
  const reasons = risk.reasons.filter((r) => r !== "nothing unusual.");
  const dests = destinationsOf(parsed);

  // ── the owner's own history ──
  const h = historyOf(parsed, memory, book);
  const { facts, newDest, unusual } = h;
  if (h.poisoning) { level = "block"; reasons.unshift(h.poisoning); }
  if (h.unusualReason) reasons.push(h.unusualReason);

  if (dests.length > 0) {
    // value leaves: silent only when every destination and the size are familiar
    if (newDest && unusual) level = worse(level, "confirm");
    else if (newDest || unusual) level = worse(level, "explain");
  }

  let sentence = explain(parsed, { origin: input.origin, intel: input.intel, addressBook: { ...labelsOf(memory, dests), ...book } }).spoken;
  if (facts.length) sentence += " " + facts.slice(0, 2).join(" ");
  if (level !== "allow" && reasons.length) sentence += " " + capital(reasons[0]!);

  const verdict: WardenVerdict = { level, sentence, reasons: level === "allow" ? [] : reasons, parsed };
  if (level === "confirm") {
    const target = newDest ?? dests[0];
    verdict.confirm = target ? { kind: "last4", expect: last4(target) } : { kind: "acknowledge", expect: "I understand" };
  }
  return verdict;
}

export interface History {
  /** Set when a destination imitates an address the owner has paid: the reason, ready to say. */
  poisoning?: string;
  /** What the owner's history says about each destination, ready to say. */
  facts: string[];
  /** A destination the owner has never paid and has not named, if any. */
  newDest: string | null;
  /** The SOL amount is more than three times the largest the owner has confirmed (needs five sends of history). */
  unusual: boolean;
  unusualReason?: string;
}

/** What the owner's own history says about this transaction. Shared by `checkTransaction` and the daemon's Signer. */
export function historyOf(parsed: ParsedTx, memory: OwnerMemory, book: Record<string, string> = {}): History {
  const out: History = { facts: [], newDest: null, unusual: false };
  for (const d of destinationsOf(parsed)) {
    const alike = memory.lookalikeOf(d);
    if (alike && !book[d]) {
      out.poisoning = out.poisoning ?? `the destination ${short(d)} looks like ${alike.label ? `"${alike.label}"` : "an address"} you have paid ${alike.timesPaid} time${alike.timesPaid === 1 ? "" : "s"}, but it is a different address. This is address poisoning.`;
      continue;
    }
    const known = memory.lookup(d);
    if (known) out.facts.push(`You've paid ${known.label ? `"${known.label}"` : short(d)} ${known.timesPaid} time${known.timesPaid === 1 ? "" : "s"} before.`);
    else if (book[d]) continue; // a contact the owner named: the sentence already uses the name
    else { out.newDest = out.newDest ?? d; out.facts.push(`You've never paid ${short(d)}.`); }
  }
  const typical = memory.typicalLamports();
  out.unusual = parsed.lamportsOut > 0n && typical.n >= MIN_HISTORY && parsed.lamportsOut > typical.max * UNUSUAL_FACTOR;
  if (out.unusual) out.unusualReason = `it sends ${fmtSol(parsed.lamportsOut)}, more than three times the most you have sent before (${fmtSol(typical.max)}).`;
  return out;
}

function labelsOf(memory: OwnerMemory, dests: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (const d of dests) { const c = memory.lookup(d); if (c?.label) out[d] = c.label; }
  return out;
}

/**
 * A `signMessage` request. The Privacy Rings key signature gets its own sentence: one signature over a fixed
 * message hands over the wallet's viewing and nullifier keys, and they can never be replaced.
 */
export function checkMessage(input: MessageInput): WardenVerdict {
  const { bytes, origin } = input;
  const who = site(origin);

  if (isRingKeyDerivation(bytes)) {
    const allowed = !!origin && originAllowed(origin, input.ringSites ?? []);
    const what = `${who} is asking for the signature that unlocks your Privacy Rings keys. Whoever holds it can read your private balance and history for good, and those keys can never be replaced.`;
    if (!allowed) return { level: "block", sentence: `${what} This site is not one you allowed to do that.`, reasons: ["it requests the Privacy Rings key-derivation signature, and the site is not on your allowed list."] };
    return { level: "confirm", sentence: `${what} You allowed this site.`, reasons: ["it requests the Privacy Rings key-derivation signature."], confirm: { kind: "acknowledge", expect: "I understand" } };
  }

  const check = checkSignMessage(bytes, input.ownKey ?? null);
  if (!check.ok) {
    const tx = bytes.length > 0 && bytes[0]! >= 0x80 && bytes[0] !== 0xff;
    return {
      level: "block",
      sentence: tx
        ? `${who} sent a transaction dressed up as a message. Signing it would execute it.`
        : `${who} sent a message I can't show you in full, such as a raw hash or an encoded order. I only pass on what you can read.`,
      reasons: [tx ? "the \"message\" is a versioned transaction message." : "the message is not readable text."],
    };
  }
  const text = Buffer.from(check.text).toString("latin1");
  const shown = text.length > 160 ? text.slice(0, 160) + "…" : text;
  return { level: "explain", sentence: `${who} asks you to sign a message. It says: "${shown.replace(/"/g, "'")}"`, reasons: [] };
}

/** After the owner confirmed and the wallet signed: the only place trust in a counterparty grows. */
export function recordConfirmed(memory: OwnerMemory, parsed: ParsedTx, at: number): void {
  const dests = destinationsOf(parsed);
  if (dests.length === 0) return;
  memory.recordConfirmedSend({ destinations: dests, lamports: parsed.lamportsOut, solDest: parsed.lamportsOut > 0n ? parsed.dest : null, programs: parsed.programs, at });
}
