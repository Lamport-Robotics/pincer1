/**
 * The signing pipeline. Every request from the extension, the agent, the swap module or the HTTP API goes
 * through here, for every chain:
 *
 *   parse → intel → explain → scam filter → presence → speak → controller → assemble
 *
 * Any step can deny. The controller is the only step that can produce a signature. The chain adapters
 * (src/chains) supply the chain-specific parse / explain / assess / claims / assemble; this file owns the
 * gates, in the same order for Solana, EVM and Bitcoin.
 */
import { Connection } from "@solana/web3.js";
import { type ParsedTx } from "./tx/parse.js";
import { type Explanation } from "./tx/explain.js";
import { type RiskVerdict } from "./risk/filter.js";
import { Intel } from "./risk/intel.js";
import type { Addresses, ControllerClient, DenyReason, MsgClaims } from "./controller/types.js";
import type { RobotLink } from "./presence/client.js";
import type { Config } from "./config.js";
import { log } from "./config.js";
import type { Settings } from "./settings.js";
import type { ChainAdapter, ChainAddressIntel, ChainId, ChainParsed, ChainTokenMeta } from "./chains/types.js";
import { SolanaAdapter, type SolanaParsed } from "./chains/solana/index.js";
import { looksLikeEvmTx } from "./chains/evm/parse.js";
import { assessPersonalMessage, assessTypedData, canonicalTypedDataJson, explainTypedData, parseTypedData, typedDataDigest, permitsOf } from "./chains/evm/message.js";

export interface SignRequest {
  /** Serialized transaction (not just the message) as produced by the dapp. */
  txBytes: Uint8Array;
  origin?: string;
  /** Requests from the overnight agent skip the "speak and wait" step when an allowance covers them. */
  source: "extension" | "agent" | "api";
}

export interface ChainSignRequest {
  chain: ChainId;
  /** Solana: serialized tx. EVM: unsigned RLP. Bitcoin: PSBT. */
  bytes: Uint8Array;
  origin?: string;
  source: "extension" | "agent" | "api";
  /** EVM: the chain id the dapp claims to be on; a mismatch with the bytes is blocked. */
  requestedChainId?: number;
  /** Spoken before the explanation (the swap module says what the owner asked for). */
  preface?: string;
  /** Extra address-book labels for this request only (the swap module names routers and pinned vaults). */
  labels?: Record<string, string>;
}

export type DenyCode = DenyReason | "blocked" | "owner_not_present" | "parse_error" | "wrong_owner" | "not_home" | "two_person_required" | "no_key" | "assemble_error";

export type SignOutcome =
  | { ok: true; signedTx: Uint8Array; parsed: ParsedTx; explanation: Explanation; risk: RiskVerdict }
  | { ok: false; reason: DenyCode; detail: string; parsed?: ParsedTx; explanation?: Explanation; risk?: RiskVerdict };

export type ChainSignOutcome<P extends ChainParsed = ChainParsed> =
  | { ok: true; signedTx: Uint8Array; parsed: P; explanation: Explanation; risk: RiskVerdict }
  | { ok: false; reason: DenyCode; detail: string; parsed?: P; explanation?: Explanation; risk?: RiskVerdict };

export interface Explained { parsed: ParsedTx; explanation: Explanation; risk: RiskVerdict }
export interface ChainExplained<P extends ChainParsed = ChainParsed> { chain: ChainId; owner: string; parsed: P; explanation: Explanation; risk: RiskVerdict; signingBytes: Uint8Array; adapter: ChainAdapter<P> }

type SignerSettings = Pick<Settings, "roomLock" | "roomMatchMin" | "twoPersonSol" | "speakBeforeSign"> & Partial<Pick<Settings, "unlimitedApproveToRouters" | "mode" | "twoPersonEvm" | "twoPersonBtc" | "twoPersonTokens" | "knownPrograms">>;

/**
 * Text a site chose (a message to sign, a memo, an origin) is quoted to the owner, never spoken as if the robot
 * meant it: control characters out, whitespace folded, length capped. The robot's own brain sees it the same way.
 */
export function quoteUntrusted(s: string, max = 120): string {
  const t = s.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, " ").replace(/\s+/g, " ").trim();
  return t.length > max ? t.slice(0, max - 1) + "…" : t;
}
const siteHost = (origin?: string) => (origin ? quoteUntrusted(origin.replace(/^https?:\/\//, ""), 60) : "");

const DENY_WORDS: Record<string, string> = { timeout: "No touch. Not signed.", claims_mismatch: "The transaction doesn't match what I read. Not signing.", over_daily_limit: "That's over your daily limit. Hold your hand on me for three seconds to raise it.", locked: "I'm locked. Tap your PIN rhythm on my head to unlock first.", policy: "That transaction asks for something I won't do.", duress: "Not now.", user: "Cancelled.", not_initialized: "I don't have a key yet.", pin_not_set: "Set your PIN rhythm first.", busy: "One thing at a time.", wrong_pin: "That's not your rhythm.", wiped: "I've been wiped. Only the 24 words can bring the wallet back." };

export class Signer {
  readonly intel: Intel;
  readonly solana: SolanaAdapter;
  private adapters = new Map<ChainId, ChainAdapter<any>>();
  /** Every address the controller can sign for; defaults to Solana only (older firmware). */
  addresses: () => Addresses;

  constructor(
    readonly cfg: Config,
    readonly controller: ControllerClient,
    readonly robot: RobotLink,
    readonly conn: Connection,
    readonly getOwner: () => string,
    intel?: Intel,
    /** How long the controller waits for a touch. */
    readonly touchTtlMs = 60_000,
    /** Owner feature toggles; everything optional defaults to off. */
    readonly settings: () => SignerSettings = () => ({ roomLock: false, roomMatchMin: 0.85, twoPersonSol: 0, speakBeforeSign: true }),
  ) {
    this.intel = intel ?? new Intel(conn);
    this.solana = new SolanaAdapter(conn, this.intel);
    this.adapters.set("solana", this.solana);
    this.addresses = () => ({ solana: getOwner() });
  }

  /** Plug in the EVM / Bitcoin adapters (main.ts) — absent adapters make those chains `no_key`. */
  register(adapter: ChainAdapter<any>) { this.adapters.set(adapter.id, adapter); }
  adapter<P extends ChainParsed = ChainParsed>(chain: ChainId): ChainAdapter<P> | null { return (this.adapters.get(chain) as ChainAdapter<P> | undefined) ?? null; }
  chains(): ChainId[] { return [...this.adapters.keys()].filter((c) => !!this.ownerFor(c)); }

  ownerFor(chain: ChainId): string | null {
    const a = this.addresses();
    return chain === "solana" ? this.getOwner() : chain === "evm" ? a.evm ?? null : a.bitcoin ?? null;
  }

  private dailyLimitFor(chain: ChainId): bigint {
    if (chain === "solana") return this.cfg.dailyLimitLamports;
    if (chain === "evm") return this.cfg.dailyLimitWei ?? 10n ** 18n;
    return this.cfg.dailyLimitSats ?? 5_000_000n;
  }

  /** The chain-generic read: parse, gather intel, explain, assess. Throws on parse errors. */
  async explainChain<P extends ChainParsed = ChainParsed>(chain: ChainId, bytes: Uint8Array, origin?: string, requestedChainId?: number, labels?: Record<string, string>): Promise<ChainExplained<P>> {
    const adapter = this.adapter<P>(chain);
    if (!adapter) throw new Error(`no adapter for ${chain}`);
    const owner = this.ownerFor(chain);
    if (!owner) throw new Error(`the controller has no ${chain} key`);
    const parsed = await adapter.parse(bytes, { owner, origin, requestedChainId });
    const intelSvc = adapter.intelFor(parsed);

    const intel: Record<string, ChainAddressIntel> = {};
    await Promise.all(parsed.counterparties.map(async (a) => { try { intel[a] = await intelSvc.address(a); } catch { /* best effort */ } }));
    const tokenMeta: Record<string, ChainTokenMeta> = {};
    const tokens = adapter.tokens?.(parsed) ?? parsed.tokenOut.map((t) => t.token).filter((t): t is string => !!t);
    await Promise.all([...new Set(tokens)].map(async (t) => { const m = await intelSvc.token(t).catch(() => null); if (m) tokenMeta[t] = m; }));
    const ownerBalance = await intelSvc.ownerBalance(owner).catch(() => null);

    const st = this.settings();
    const ctx = { origin, intel, tokenMeta, addressBook: labels ? { ...this.cfg.addressBook, ...labels } : this.cfg.addressBook, ownerBalance };
    const explanation = adapter.explain(parsed, ctx);
    // the owner's hand-approved list is the only thing that makes a program "known"; signing once does not
    const knownPrograms = new Set((st.knownPrograms ?? []).map((p) => (chain === "evm" ? p.toLowerCase() : p)));
    const risk = adapter.assess(parsed, { ...ctx, owner, originAllowlist: this.cfg.originAllowlist, dailyLimit: this.dailyLimitFor(chain), knownPrograms, routers: adapter.routers ?? new Set(), unlimitedApproveToRouters: !!st.unlimitedApproveToRouters });
    return { chain, owner, parsed, explanation, risk, signingBytes: adapter.signingBytes(bytes, parsed), adapter };
  }

  /** Solana, unchanged contract: the explanation and the message bytes the controller signs. */
  async explainTx(txBytes: Uint8Array, origin?: string): Promise<Explained & { messageBytes: Uint8Array }> {
    const ex = await this.explainChain<SolanaParsed>("solana", txBytes, origin);
    return { parsed: ex.parsed.tx, explanation: ex.explanation, risk: ex.risk, messageBytes: ex.parsed.messageBytes };
  }

  /** The gates, for every chain. */
  async signChain<P extends ChainParsed = ChainParsed>(req: ChainSignRequest): Promise<ChainSignOutcome<P>> {
    if (!this.adapter(req.chain) || !this.ownerFor(req.chain)) return { ok: false, reason: "no_key", detail: `the controller has no ${req.chain} key` };
    let ex: ChainExplained<P>;
    try {
      ex = await this.explainChain<P>(req.chain, req.bytes, req.origin, req.requestedChainId, req.labels);
    } catch (e) {
      return { ok: false, reason: "parse_error", detail: String((e as Error).message ?? e) };
    }
    const { parsed, explanation, risk, owner, adapter } = ex;
    const same = (a: string, b: string) => (req.chain === "evm" ? a.toLowerCase() === b.toLowerCase() : a === b);
    if (!parsed.signers.some((s) => same(s, owner))) return { ok: false, reason: "wrong_owner", detail: req.chain === "bitcoin" ? "none of the coins being spent are mine" : "this transaction is not for my key", parsed, explanation, risk };

    if (risk.level === "block") {
      const why = risk.reasons[0];
      await this.robot.say(`I'm not signing this. ${why}`);
      log.warn("blocked", { chain: req.chain, origin: req.origin, why });
      return { ok: false, reason: "blocked", detail: why, parsed, explanation, risk };
    }

    // the overnight agent is the one caller that runs while the owner sleeps: its requests skip the presence
    // check in every mode, and what they may sign is bounded by the allowance the owner granted with a long
    // touch (the controller signs nothing else without a touch). Everything else needs the owner in the room.
    if (req.source !== "agent" && !this.robot.ownerPresent()) {
      await this.robot.say("Someone wants a signature, but I can't see you. Come over and look at me.");
      return { ok: false, reason: "owner_not_present", detail: "owner not verified in the room", parsed, explanation, risk };
    }

    const st = this.settings();
    const pres = this.robot.state();
    // room lock: an unknown room match (lidar not reporting) is not home either
    if (req.source !== "agent" && st.roomLock && (pres.room_match == null || pres.room_match < st.roomMatchMin)) {
      await this.robot.say("This is not my room. I don't sign anywhere but home.");
      return { ok: false, reason: "not_home", detail: pres.room_match == null ? "room match unknown" : `room match ${pres.room_match.toFixed(2)} below ${st.roomMatchMin}`, parsed, explanation, risk };
    }
    const twoPerson = req.source === "agent" ? null : this.twoPersonReason(req.chain, parsed, st);
    if (twoPerson) {
      const owners = pres.owners ?? [];
      if ((pres.persons ?? 0) < 2 || owners.length < 2) {
        await this.robot.say(`${twoPerson} For that I need both of you in the room.`);
        return { ok: false, reason: "two_person_required", detail: `persons=${pres.persons ?? 0} owners=${owners.length}`, parsed, explanation, risk };
      }
    }

    if (req.source !== "agent") {
      await this.robot.attention();
      const lead = risk.level === "high" ? `Careful. ${risk.reasons[0]} ` : risk.level === "medium" ? `${risk.reasons[0]} ` : "";
      if (st.speakBeforeSign) await this.robot.say(`${req.preface ? req.preface + " " : ""}${lead}${quoteUntrusted(explanation.spoken, 400)} Touch me to sign.`);
    }

    const claims = adapter.claims(parsed);
    // The controller's own reading goes on its trusted display; if it disagrees with what we just said, say so.
    const onPending = (p: { need: string; parsed?: Record<string, unknown> }) => {
      const cp = p.parsed; if (!cp) return;
      const ours = (claims as any).lamports_out ?? (claims as any).value_wei ?? (claims as any).sats_out;
      const theirs = cp.lamports_out ?? cp.value_wei ?? cp.sats_out;
      if (ours != null && theirs != null && String(ours) !== String(theirs)) {
        log.warn("controller reads the transaction differently", { ours: String(ours), theirs: String(theirs) });
        if (req.source !== "agent") void this.robot.say("My screen shows a different amount than I just said. Trust the screen.");
      }
    };
    const res = await this.controller.sign(adapter.signKind, ex.signingBytes, claims, this.touchTtlMs, onPending);
    if (!res.ok) {
      if (req.source !== "agent") await this.robot.say(DENY_WORDS[res.reason] ?? "Not signed.");
      return { ok: false, reason: res.reason, detail: res.reason, parsed, explanation, risk };
    }

    let signedTx: Uint8Array;
    try { signedTx = await adapter.assemble(req.bytes, res.sig, parsed, owner); } catch (e) {
      log.error("assemble failed", { chain: req.chain, err: String((e as Error).message ?? e) });
      await this.robot.say("The controller's signature doesn't fit the transaction. Nothing was sent.");
      return { ok: false, reason: "assemble_error", detail: String((e as Error).message ?? e), parsed, explanation, risk };
    }
    if (req.source !== "agent") await this.robot.say("Signed.");
    return { ok: true, signedTx, parsed, explanation, risk };
  }

  /** Why this request needs two owners present, or null. Native thresholds per chain; tokens by switch (no oracle). */
  private twoPersonReason(chain: ChainId, parsed: ChainParsed, st: SignerSettings): string | null {
    const n = Number(parsed.nativeOut);
    if (chain === "solana" && st.twoPersonSol > 0 && n / 1e9 >= st.twoPersonSol) return `That's ${n / 1e9} SOL.`;
    if (chain === "evm" && (st.twoPersonEvm ?? 0) > 0 && n / 1e18 >= (st.twoPersonEvm ?? 0)) return `That's ${n / 1e18} of the native coin.`;
    if (chain === "bitcoin" && (st.twoPersonBtc ?? 0) > 0 && n / 1e8 >= (st.twoPersonBtc ?? 0)) return `That's ${n / 1e8} BTC.`;
    if (st.twoPersonTokens && parsed.tokenOut.length > 0) return "Tokens are leaving the wallet.";
    return null;
  }

  /** Solana, unchanged contract. */
  async sign(req: SignRequest): Promise<SignOutcome> {
    const r = await this.signChain<SolanaParsed>({ chain: "solana", bytes: req.txBytes, origin: req.origin, source: req.source });
    if (r.ok) return { ok: true, signedTx: r.signedTx, parsed: r.parsed.tx, explanation: r.explanation, risk: r.risk };
    return { ok: false, reason: r.reason, detail: r.detail, parsed: r.parsed?.tx, explanation: r.explanation, risk: r.risk };
  }

  async broadcast(chain: ChainId, signedTx: Uint8Array, parsed?: ChainParsed): Promise<string> {
    const adapter = this.adapter(chain);
    if (!adapter) throw new Error(`no adapter for ${chain}`);
    return adapter.broadcast(signedTx, parsed);
  }

  async signMessage(message: Uint8Array, origin?: string): Promise<{ ok: true; signature: Uint8Array } | { ok: false; reason: string }> {
    if (!this.robot.ownerPresent()) { await this.robot.say("Someone wants me to sign a message, but I can't see you."); return { ok: false, reason: "owner_not_present" }; }
    const text = quoteUntrusted(new TextDecoder().decode(message));
    await this.robot.say(`${origin ? siteHost(origin) + " asks me to sign a message." : "Someone asks me to sign a message."} The site says: "${text}". Touch me to sign.`);
    const res = await this.controller.sign("msg", message, { lamports_out: "0", token_out: [], dest: "", programs: [], dangerous: [] }, this.touchTtlMs);
    if (!res.ok) { await this.robot.say(res.reason === "timeout" ? "No touch. Not signed." : "Not signing that."); return { ok: false, reason: res.reason }; }
    await this.robot.say("Signed.");
    return { ok: true, signature: res.sig };
  }

  /**
   * EVM messages. `message`: EIP-191 personal_sign bytes. `typedData`: EIP-712 JSON (eth_signTypedData_v4).
   * Permits are explained and filtered like approvals; the controller re-hashes the typed data itself.
   */
  async signEvmMessage(req: { message?: Uint8Array; typedData?: string | object; origin?: string; requestedChainId?: number }): Promise<{ ok: true; signature: Uint8Array; explanation: Explanation; risk: RiskVerdict } | { ok: false; reason: DenyCode; detail: string; explanation?: Explanation; risk?: RiskVerdict }> {
    const owner = this.ownerFor("evm");
    const adapter = this.adapter("evm");
    if (!owner || !adapter) return { ok: false, reason: "no_key", detail: "the controller has no evm key" };
    let bytes: Uint8Array; let claims: MsgClaims; let explanation: Explanation; let risk: RiskVerdict;
    try {
      if (req.typedData != null) {
        const td = parseTypedData(req.typedData);
        const digest = typedDataDigest(td);
        bytes = new Uint8Array(Buffer.from(digest.slice(2), "hex"));
        claims = { kind: "eip712", typed_data: canonicalTypedDataJson(td), digest };
        const chainId = td.domain.chainId != null ? Number(td.domain.chainId) : req.requestedChainId;
        const intel: Record<string, ChainAddressIntel> = {};
        const tokenMeta: Record<string, ChainTokenMeta> = {};
        const evm = adapter as unknown as { intelForChain?: (id: number) => { address(a: string): Promise<ChainAddressIntel>; token(a: string): Promise<ChainTokenMeta | null> }; hasChain?: (id: number) => boolean };
        const svc = chainId != null && evm.hasChain?.(chainId) ? evm.intelForChain?.(chainId) : undefined;
        if (svc) {
          const vc = typeof td.domain.verifyingContract === "string" ? td.domain.verifyingContract : null;
          if (vc) { try { intel[vc] = await svc.address(vc); } catch { /* best effort */ } }
          await Promise.all(permitsOf(td).flatMap((p) => [
            (async () => { try { intel[p.spender] = await svc.address(p.spender); } catch { /* best effort */ } })(),
            (async () => { if (p.token) { const m = await svc.token(p.token).catch(() => null); if (m) tokenMeta[p.token] = m; } })(),
          ]));
        }
        explanation = explainTypedData(td, { origin: req.origin, intel, tokenMeta, addressBook: this.cfg.addressBook });
        risk = assessTypedData(td, { intel, routers: adapter.routers, requestedChainId: req.requestedChainId });
      } else if (req.message) {
        bytes = req.message;
        claims = { kind: "eip191" };
        const text = quoteUntrusted(new TextDecoder().decode(bytes));
        explanation = { spoken: `${req.origin ? siteHost(req.origin) + " asks me to sign a message." : "Someone asks me to sign a message."} The site says: "${text}"`, lines: [`message: ${text}`] };
        risk = assessPersonalMessage(bytes, looksLikeEvmTx(bytes));
      } else return { ok: false, reason: "parse_error", detail: "message or typedData required" };
    } catch (e) { return { ok: false, reason: "parse_error", detail: String((e as Error).message ?? e) }; }

    if (risk.level === "block") { await this.robot.say(`I'm not signing this. ${risk.reasons[0]}`); return { ok: false, reason: "blocked", detail: risk.reasons[0], explanation, risk }; }
    if (!this.robot.ownerPresent()) { await this.robot.say("Someone wants me to sign a message, but I can't see you."); return { ok: false, reason: "owner_not_present", detail: "owner not verified in the room", explanation, risk }; }
    const lead = risk.level === "high" ? `Careful. ${risk.reasons[0]} ` : risk.level === "medium" ? `${risk.reasons[0]} ` : "";
    await this.robot.attention();
    if (this.settings().speakBeforeSign) await this.robot.say(`${lead}${explanation.spoken.replace(/\.$/, "")}. Touch me to sign.`);
    const res = await this.controller.sign("evm_msg", bytes, claims, this.touchTtlMs);
    if (!res.ok) { await this.robot.say(DENY_WORDS[res.reason] ?? "Not signing that."); return { ok: false, reason: res.reason, detail: res.reason, explanation, risk }; }
    await this.robot.say("Signed.");
    return { ok: true, signature: res.sig, explanation, risk };
  }
}
