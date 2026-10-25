/**
 * In-process simulation of the consent controller. Same protocol semantics as the firmware
 * (docs/PROTOCOL.md §1) with software keys, so the whole stack runs on a laptop and in tests.
 *
 * The sim enforces the same checks as the firmware's policy.c, computed with the daemon's own parsers:
 * ed25519 for Solana, secp256k1 (via @noble/curves, sim only) for EVM and Bitcoin. In production
 * `hello.se050` is false for the sim, and the daemon refuses to start.
 */
import nacl from "tweetnacl";
import bs58 from "bs58";
import { createHash } from "node:crypto";
import { secp256k1 } from "@noble/curves/secp256k1";
import { getAddress, hashMessage, keccak256, type Hex } from "viem";
import { publicKeyToAddress } from "viem/accounts";
import * as bitcoin from "bitcoinjs-lib";
import { parseMessage, looksLikeMessage } from "../tx/parse.js";
import { looksLikeEvmTx, parseEvmTx } from "../chains/evm/parse.js";
import { parseTypedData, typedDataDigest } from "../chains/evm/message.js";
import { netOf, parsePsbt, type BtcNetwork } from "../chains/bitcoin/psbt.js";
import type { Addresses, Allowance, AnyClaims, BtcClaims, Claims, ControllerClient, ControllerEvent, DenyReason, EvmClaims, Hello, InheritanceStatus, MsgClaims, PendingHook, Policy, PolicyChain, SignKind, UpdateBegin, UpdateReason, UpdateResult } from "./types.js";
import { FW_CHUNK, FW_PAGE, checkImage } from "../firmware.js";
import { SYSTEM } from "../tx/known.js";
import { EVM_ROUTERS } from "../swap/routers.js";

export interface SimOptions {
  seed?: Uint8Array; // 32 bytes
  dailyLimitLamports?: bigint;
  dailyLimitWei?: bigint;
  dailyLimitSats?: bigint;
  btcNetwork?: BtcNetwork;
  /** When true, every pending request is approved immediately (tests). */
  autoTouch?: boolean;
  /** Long-touch requests too. */
  autoLongTouch?: boolean;
  initialized?: boolean;
}

interface Pending { id: string; need: "touch" | "long_touch" | "pin"; resolve: (ok: boolean) => void; timer: NodeJS.Timeout }

type SignResult = { ok: true; sig: Uint8Array } | { ok: false; reason: DenyReason };

/** One key family set: ed25519 for Solana, one secp256k1 key for EVM and one for Bitcoin. */
interface KeySet { sol: nacl.SignKeyPair; evm: Uint8Array; btc: Uint8Array }

const derive = (seed: Uint8Array | undefined, label: string): Uint8Array => {
  if (!seed) return secp256k1.utils.randomPrivateKey();
  const k = createHash("sha256").update(Buffer.concat([Buffer.from(seed), Buffer.from(label)])).digest();
  return secp256k1.utils.isValidPrivateKey(k) ? new Uint8Array(k) : secp256k1.utils.randomPrivateKey();
};
const makeKeys = (seed?: Uint8Array): KeySet => ({ sol: seed ? nacl.sign.keyPair.fromSeed(seed) : nacl.sign.keyPair(), evm: derive(seed, "pincer-evm"), btc: derive(seed, "pincer-btc") });
const evmAddressOf = (priv: Uint8Array) => publicKeyToAddress(("0x" + Buffer.from(secp256k1.getPublicKey(priv, false)).toString("hex")) as Hex);
const btcAddressOf = (priv: Uint8Array, network: BtcNetwork) => bitcoin.payments.p2wpkh({ pubkey: Buffer.from(secp256k1.getPublicKey(priv, true)), network: netOf(network) }).address!;
/** 65-byte r‖s‖v. legacyV: 27/28 (legacy transactions and messages), otherwise 0/1 (typed transactions). */
const sig65 = (digest: Uint8Array, priv: Uint8Array, legacyV: boolean): Uint8Array => {
  const s = secp256k1.sign(digest, priv, { lowS: true });
  const out = new Uint8Array(65);
  out.set(s.toCompactRawBytes(), 0);
  out[64] = (legacyV ? 27 : 0) + (s.recovery ?? 0);
  return out;
};
const hexBytes = (h: Hex) => new Uint8Array(Buffer.from(h.slice(2), "hex"));

export class SimController implements ControllerClient {
  private real: KeySet;
  private decoy: KeySet;
  private initialized: boolean;
  private pinSet = true;
  private locked = false;
  private duressActive = false;
  private duressPinSet = false;
  /** Which rhythm the test "tapped" for the pending PIN request: real, duress or wrong. */
  private nextRhythm: "real" | "duress" | "wrong" = "real";
  private policy: Policy;
  private allow: Allowance | null = null;
  private elevatedUntil = 0;
  /** router allowlist lives here, not on the host */
  readonly routers = new Set<string>(EVM_ROUTERS.map((r) => r.toLowerCase()));
  private elevatedLimit = 0n;
  private pend: Pending | null = null;
  private listeners: ((e: ControllerEvent) => void)[] = [];
  private nowUnix = Math.floor(Date.now() / 1000);
  /** Powered-uptime seconds, the controller's own clock for the dead-man's switch (tests drive it). */
  poweredSeconds = 0;
  private lastTouchSeconds = 0;
  private inh: { bytes: Uint8Array; sig: Uint8Array; inactivityDays: number; warnDays: number[]; heir: string; warned: number[] } | null = null;
  readonly opts: SimOptions;
  readonly btcNetwork: BtcNetwork;

  constructor(opts: SimOptions = {}) {
    this.opts = opts;
    this.real = makeKeys(opts.seed);
    this.decoy = makeKeys();
    this.initialized = opts.initialized ?? true;
    this.pinSet = this.initialized;
    this.btcNetwork = opts.btcNetwork ?? "mainnet";
    this.policy = {
      daily_limit_lamports: (opts.dailyLimitLamports ?? 2_000_000_000n).toString(), spent_today_lamports: "0", day: Math.floor(this.nowUnix / 86_400),
      daily_limit_wei: (opts.dailyLimitWei ?? 10n ** 18n).toString(), spent_today_wei: "0",
      daily_limit_sats: (opts.dailyLimitSats ?? 5_000_000n).toString(), spent_today_sats: "0",
    };
  }

  private get keys() { return this.duressActive ? this.decoy : this.real; }
  get publicKey(): string { return bs58.encode(this.keys.sol.publicKey); }
  get realPublicKey(): string { return bs58.encode(this.real.sol.publicKey); }
  get evmAddress(): string { return evmAddressOf(this.keys.evm); }
  get bitcoinAddress(): string { return btcAddressOf(this.keys.btc, this.btcNetwork); }
  get addresses(): Addresses { return { solana: this.publicKey, evm: this.evmAddress, bitcoin: this.bitcoinAddress }; }
  private addressesOf(k: KeySet): Addresses { return { solana: bs58.encode(k.sol.publicKey), evm: evmAddressOf(k.evm), bitcoin: btcAddressOf(k.btc, this.btcNetwork) }; }
  /** Test hook: the rhythm the owner taps for the next PIN request. */
  rhythm(kind: "real" | "duress" | "wrong") { this.nextRhythm = kind; }
  /** Compressed secp256k1 public key of the Bitcoin slot (tests verify signatures with it). */
  get bitcoinPubkey(): Uint8Array { return secp256k1.getPublicKey(this.keys.btc, true); }

  /** Simulated physical touch. */
  touch(long = false) {
    this.lastTouchSeconds = this.poweredSeconds;
    this.emit({ t: "event", ev: long ? "long_touch" : "touch" });
    if (!this.pend) return;
    if (this.pend.need !== "touch" && !long) return;
    const p = this.pend; this.pend = null; clearTimeout(p.timer); p.resolve(true);
  }

  private emit(e: ControllerEvent) { for (const l of this.listeners) l(e); }
  onEvent(cb: (e: ControllerEvent) => void) { this.listeners.push(cb); }
  pending() { return this.pend ? { id: this.pend.id, need: this.pend.need } : null; }

  private waitTouch(id: string, need: "touch" | "long_touch" | "pin", ttlMs: number): Promise<boolean> {
    if (this.pend) return Promise.resolve(false);
    return new Promise((resolve) => {
      const timer = setTimeout(() => { if (this.pend?.id === id) { this.pend = null; resolve(false); } }, ttlMs);
      this.pend = { id, need, resolve, timer };
      if ((need === "touch" && this.opts.autoTouch) || (need !== "touch" && (this.opts.autoLongTouch ?? this.opts.autoTouch))) setImmediate(() => this.touch(need !== "touch"));
    });
  }

  private rollDay() {
    const day = Math.floor(this.nowUnix / 86_400);
    if (day !== this.policy.day) { this.policy.day = day; this.policy.spent_today_lamports = "0"; this.policy.spent_today_wei = "0"; this.policy.spent_today_sats = "0"; }
  }

  /** Advance the simulated powered clock; emits inheritance warnings like the firmware does. */
  tick(seconds: number) {
    this.poweredSeconds += seconds;
    if (!this.inh) return;
    const left = this.inh.inactivityDays - (this.poweredSeconds - this.lastTouchSeconds) / 86_400;
    for (const w of this.inh.warnDays) if (left <= w && !this.inh.warned.includes(w)) { this.inh.warned.push(w); this.emit({ t: "event", ev: "inheritance_warning", days_left: w }); }
  }
  private daysSinceTouch() { return (this.poweredSeconds - this.lastTouchSeconds) / 86_400; }

  async inheritanceSet(bytes: Uint8Array, claims: Claims, cfg: { inactivityDays: number; warnDays: number[]; heir: string }) {
    if (!this.initialized) return { ok: false as const, reason: "not_initialized" as const };
    let parsed;
    try { parsed = parseMessage(bytes, { owner: this.realPublicKey }); } catch { return { ok: false as const, reason: "policy" as const }; }
    const first = parsed.instructions[0];
    if (!first || first.program !== SYSTEM || first.label !== "System.advanceNonce" || first.accounts[0] === this.realPublicKey) return { ok: false as const, reason: "not_durable_nonce" as const };
    if (!parsed.signers.includes(this.realPublicKey)) return { ok: false as const, reason: "claims_mismatch" as const };
    if (parsed.lamportsOut > BigInt(claims.lamports_out)) return { ok: false as const, reason: "claims_mismatch" as const };
    if (!parsed.programs.every((p) => claims.programs.includes(p))) return { ok: false as const, reason: "claims_mismatch" as const };
    const ok = await this.waitTouch(`h${Date.now()}`, "long_touch", 60_000);
    if (!ok) return { ok: false as const, reason: "timeout" as const };
    const sig = nacl.sign.detached(bytes, this.real.sol.secretKey);
    this.inh = { bytes, sig, inactivityDays: cfg.inactivityDays, warnDays: [...cfg.warnDays].sort((a, b) => b - a), heir: cfg.heir, warned: [] };
    return { ok: true as const, sig };
  }
  async inheritanceStatus(): Promise<InheritanceStatus> {
    return { armed: !!this.inh, inactivity_days: this.inh?.inactivityDays ?? 0, days_since_touch: this.daysSinceTouch(), powered_days: this.poweredSeconds / 86_400, heir: this.inh?.heir ?? null, warned: this.inh?.warned ?? [] };
  }
  async inheritanceRelease() {
    if (!this.inh) return { ok: false as const, reason: "not_armed" as const };
    if (this.daysSinceTouch() < this.inh.inactivityDays) return { ok: false as const, reason: "not_expired" as const };
    // signed tx = [1 signature][message]
    const tx = new Uint8Array(1 + 64 + this.inh.bytes.length);
    tx[0] = 1; tx.set(this.inh.sig, 1); tx.set(this.inh.bytes, 65);
    return { ok: true as const, tx };
  }
  async inheritanceClear() { const ok = await this.waitTouch(`hc${Date.now()}`, "long_touch", 60_000); if (ok) this.inh = null; return ok; }

  async time(unix: number) { this.nowUnix = unix; this.rollDay(); if (this.allow && unix > this.allow.until_unix) this.allow = null; }

  async hello(): Promise<Hello> {
    this.rollDay();
    return { t: "hello", fw: this.fw, se050: false, initialized: this.initialized, pubkey: this.initialized ? this.publicKey : null, addresses: this.initialized ? this.addresses : undefined, locked: this.locked, policy: { ...this.policy }, allowance: this.allow ? { ...this.allow } : null, duress_pin_set: this.duressPinSet, pin_set: this.pinSet, pin_failures: 0, dev_keys: false, display: false, board: "sim", inheritance: this.inh ? { armed: true, inactivity_days: this.inh.inactivityDays, days_since_touch: this.daysSinceTouch() } : null, update: { partition: this.partition, staged: this.upd?.state === "verified", trial: this.trial } };
  }

  async init() {
    if (this.initialized) throw new Error("already initialized");
    const ok = await this.waitTouch("init", "long_touch", 60_000);
    if (!ok) throw new Error("timeout");
    this.initialized = true;
    // The real controller shows the 24 words on its own display; the sim has no display and no words to show.
    // The decoy's public addresses come back once so the owner can fund it.
    return { pubkey: this.publicKey, addresses: this.addresses, decoy_addresses: this.addressesOf(this.decoy) };
  }

  /** Daily limit for a chain, honouring a pending elevation (which applies to the next signature, any chain). */
  private limitFor(chain: PolicyChain): bigint {
    if (this.elevatedUntil > Date.now()) return this.elevatedLimit;
    return BigInt(chain === "solana" ? this.policy.daily_limit_lamports : chain === "evm" ? this.policy.daily_limit_wei ?? "0" : this.policy.daily_limit_sats ?? "0");
  }
  private spentFor(chain: PolicyChain): bigint { return BigInt(chain === "solana" ? this.policy.spent_today_lamports : chain === "evm" ? this.policy.spent_today_wei ?? "0" : this.policy.spent_today_sats ?? "0"); }
  private addSpent(chain: PolicyChain, n: bigint) {
    const v = (this.spentFor(chain) + n).toString();
    if (chain === "solana") this.policy.spent_today_lamports = v; else if (chain === "evm") this.policy.spent_today_wei = v; else this.policy.spent_today_sats = v;
  }

  async sign(kind: SignKind, bytes: Uint8Array, claims: AnyClaims, ttlMs = 60_000, onPending?: PendingHook): Promise<SignResult> {
    this.rollDay();
    if (!this.initialized) return { ok: false, reason: "not_initialized" };
    if (!this.pinSet) return { ok: false, reason: "pin_not_set" };
    if (this.locked) return { ok: false, reason: "locked" };
    switch (kind) {
      case "tx": case "msg": return this.signSolana(kind, bytes, claims as Claims, ttlMs, onPending);
      case "evm_tx": return this.signEvm(bytes, claims as EvmClaims, ttlMs, onPending);
      case "evm_msg": return this.signEvmMessage(bytes, claims as MsgClaims, ttlMs);
      case "btc_psbt": return this.signBtc(bytes, claims as BtcClaims, ttlMs);
      default: return { ok: false, reason: "policy" };
    }
  }

  // ── Solana (unchanged semantics) ──
  private async signSolana(kind: "tx" | "msg", bytes: Uint8Array, claims: Claims, ttlMs: number, onPending?: PendingHook): Promise<SignResult> {
    const kp = this.keys.sol;
    const owner = bs58.encode(kp.publicKey);
    let lamportsOut = 0n;
    let lastDest: string | null = null;
    let needTouch = true;

    if (kind === "msg") {
      if (looksLikeMessage(bytes)) return { ok: false, reason: "policy" };
    } else {
      let parsed;
      try { parsed = parseMessage(bytes, { owner }); } catch { return { ok: false, reason: "policy" }; }
      // 1. owner must be the fee payer / a signer
      if (!parsed.signers.includes(owner)) return { ok: false, reason: "claims_mismatch" };
      // 2. lamports out ≤ claimed
      lamportsOut = parsed.lamportsOut;
      lastDest = parsed.dest;
      if (lamportsOut > BigInt(claims.lamports_out)) return { ok: false, reason: "claims_mismatch" };
      // the fee is SOL leaving too: an unclaimed priority fee is a transfer to the leader the display never showed
      if (parsed.feeLamports > BigInt(claims.fee_lamports ?? "0")) return { ok: false, reason: "claims_mismatch" };
      lamportsOut += parsed.priorityLamports;
      // dest must match the claim
      if (lamportsOut > 0n && parsed.dest && parsed.dest !== claims.dest) return { ok: false, reason: "claims_mismatch" };
      // every token out needs a matching claim (mint, amount, dest)
      for (const t of parsed.tokenOut) {
        const c = claims.token_out.find((x) => (x.mint ?? "") === (t.mint ?? "") && (x.dest ?? "") === (t.dest ?? ""));
        if (!c || t.amount > BigInt(c.amount)) return { ok: false, reason: "claims_mismatch" };
      }
      // 3. every program claimed
      if (!parsed.programs.every((p) => claims.programs.includes(p))) return { ok: false, reason: "claims_mismatch" };
      // 4. dangerous instructions must be declared
      const kinds = new Set(parsed.dangers.map((d) => d.kind));
      for (const k of kinds) if (!claims.dangerous.includes(k)) return { ok: false, reason: "policy" };
      // 5. daily limit / elevation / allowance
      if (this.spentFor("solana") + lamportsOut > this.limitFor("solana")) return { ok: false, reason: "over_daily_limit" };
      // allowance: sign without touch when it fits
      const a = this.allow;
      if (a && this.nowUnix <= a.until_unix && kinds.size === 0 && parsed.programs.every((p) => a.programs.includes(p)) && lamportsOut <= BigInt(a.max_per_tx_lamports) && BigInt(a.spent) + lamportsOut <= BigInt(a.lamports)) {
        needTouch = false;
        a.spent = (BigInt(a.spent) + lamportsOut).toString();
      }
    }

    if (needTouch) {
      onPending?.({ need: "touch", parsed: { lamports_out: lamportsOut.toString(), dest: lastDest ?? "" } });
      const ok = await this.waitTouch(`s${Date.now()}`, "touch", ttlMs);
      if (!ok) return { ok: false, reason: "timeout" };
    }
    this.addSpent("solana", lamportsOut);
    this.elevatedUntil = 0;
    return { ok: true, sig: nacl.sign.detached(bytes, kp.secretKey) };
  }

  /** Owner adds a router to the controller's allowlist: 3-second touch (the real controller also asks for the PIN rhythm). */
  async addRouter(address: string, ttlMs = 60_000): Promise<boolean> {
    if (!/^0x[0-9a-fA-F]{40}$/.test(address)) return false;
    const ok = await this.waitTouch(`router${Date.now()}`, "long_touch", ttlMs);
    if (!ok) return false;
    this.routers.add(address.toLowerCase());
    return true;
  }

  // ── EVM: PROTOCOL §1 checks (1)–(6) ──
  private async signEvm(bytes: Uint8Array, claims: EvmClaims, ttlMs: number, onPending?: PendingHook): Promise<SignResult> {
    const priv = this.keys.evm;
    const owner = evmAddressOf(priv);
    let parsed;
    // `claims.routers` is ignored on purpose: a compromised host must not be able to bless its own router.
    try { parsed = parseEvmTx(bytes, { owner }, { routers: this.routers }); } catch { return { ok: false, reason: "policy" }; }
    // (1) chain id
    if (parsed.chainId !== Number(claims.chain_id)) return { ok: false, reason: "claims_mismatch" };
    // destination must be what was described
    if ((parsed.to ?? "").toLowerCase() !== String(claims.to ?? "").toLowerCase()) return { ok: false, reason: "claims_mismatch" };
    // (2) value
    if (parsed.value > BigInt(claims.value_wei)) return { ok: false, reason: "claims_mismatch" };
    // (3) worst-case fee
    if (parsed.maxFeeWei > BigInt(claims.max_fee_wei)) return { ok: false, reason: "claims_mismatch" };
    // (4) every ERC-20 amount leaving the owner ≤ claimed
    for (const t of parsed.tokenOut) {
      const c = (claims.token_out ?? []).find((x) => x.token.toLowerCase() === (t.token ?? "").toLowerCase());
      if (!c || t.amount > BigInt(c.amount)) return { ok: false, reason: "claims_mismatch" };
      // the ERC-20 recipient must be the one described
      if (t.dest && (c.to ?? "").toLowerCase() !== t.dest.toLowerCase()) return { ok: false, reason: "claims_mismatch" };
    }
    // (5) dangers declared
    for (const d of parsed.dangers) if (!(claims.dangerous ?? []).includes(d.kind as EvmClaims["dangerous"][number])) return { ok: false, reason: "policy" };
    // (6) daily limit in wei
    if (this.spentFor("evm") + parsed.value > this.limitFor("evm")) return { ok: false, reason: "over_daily_limit" };

    onPending?.({ need: "touch", parsed: { chain_id: parsed.chainId, to: parsed.to ?? "", value_wei: parsed.value.toString(), dangerous: parsed.dangers.map((d) => d.kind) } });
    const ok = await this.waitTouch(`e${Date.now()}`, "touch", ttlMs);
    if (!ok) return { ok: false, reason: "timeout" };
    this.addSpent("evm", parsed.value);
    this.elevatedUntil = 0;
    const digest = hexBytes(keccak256(("0x" + Buffer.from(bytes).toString("hex")) as Hex));
    return { ok: true, sig: sig65(digest, priv, parsed.type === "legacy") };
  }

  // ── EVM messages: EIP-191 bytes or an EIP-712 digest the controller re-derives from the JSON ──
  private async signEvmMessage(bytes: Uint8Array, claims: MsgClaims, ttlMs: number): Promise<SignResult> {
    const priv = this.keys.evm;
    let digest: Uint8Array;
    if (claims.kind === "eip712") {
      let td;
      try { td = parseTypedData(claims.typed_data); } catch { return { ok: false, reason: "policy" }; }
      let computed: Hex;
      try { computed = typedDataDigest(td); } catch { return { ok: false, reason: "policy" }; }
      if (bytes.length !== 32 || Buffer.from(bytes).toString("hex") !== computed.slice(2)) return { ok: false, reason: "claims_mismatch" };
      if (claims.digest && claims.digest.toLowerCase() !== computed.toLowerCase()) return { ok: false, reason: "claims_mismatch" };
      digest = bytes;
    } else {
      // a "message" must never be a transaction in disguise
      if (looksLikeEvmTx(bytes) || looksLikeMessage(bytes)) return { ok: false, reason: "policy" };
      digest = hexBytes(hashMessage({ raw: bytes }));
    }
    const ok = await this.waitTouch(`m${Date.now()}`, "touch", ttlMs);
    if (!ok) return { ok: false, reason: "timeout" };
    return { ok: true, sig: sig65(digest, priv, true) };
  }

  // ── Bitcoin: PROTOCOL §1 checks (1)–(6), then partial signatures on every owner input ──
  private async signBtc(bytes: Uint8Array, claims: BtcClaims, ttlMs: number): Promise<SignResult> {
    const priv = this.keys.btc;
    const owner = btcAddressOf(priv, this.btcNetwork);
    // (1) network
    if (claims.network !== this.btcNetwork) return { ok: false, reason: "claims_mismatch" };
    let parsed;
    try { parsed = parsePsbt(bytes, owner, this.btcNetwork); } catch { return { ok: false, reason: "policy" }; }
    if (parsed.ownerInputs.length === 0 || parsed.inputsWithoutValue > 0 || parsed.feeSats < 0n) return { ok: false, reason: "claims_mismatch" };
    if (parsed.ownerInputs.length !== Number(claims.inputs)) return { ok: false, reason: "claims_mismatch" };
    // (2) non-change outputs ≤ claimed; destination as described
    if (parsed.satsOut > BigInt(claims.sats_out)) return { ok: false, reason: "claims_mismatch" };
    if ((parsed.dest ?? "") !== (claims.dest ?? "")) return { ok: false, reason: "claims_mismatch" };
    // (3) fee ≤ claimed
    if (parsed.feeSats > BigInt(claims.fee_sats)) return { ok: false, reason: "claims_mismatch" };
    // (4)(5) unknown outputs and non-ALL sighash must be declared; so must a fee above 5%
    for (const d of parsed.dangers) if (!(claims.dangerous ?? []).includes(d.kind as BtcClaims["dangerous"][number])) return { ok: false, reason: "policy" };
    if (parsed.satsOut > 0n && Number(parsed.feeSats) / Number(parsed.satsOut) > 0.05 && !(claims.dangerous ?? []).includes("high_fee")) return { ok: false, reason: "policy" };
    // (6) daily limit in sats
    if (this.spentFor("bitcoin") + parsed.satsOut > this.limitFor("bitcoin")) return { ok: false, reason: "over_daily_limit" };

    const ok = await this.waitTouch(`b${Date.now()}`, "touch", ttlMs);
    if (!ok) return { ok: false, reason: "timeout" };
    this.addSpent("bitcoin", parsed.satsOut);
    this.elevatedUntil = 0;
    const psbt = bitcoin.Psbt.fromBuffer(Buffer.from(bytes), { network: netOf(this.btcNetwork) });
    const signer = { publicKey: Buffer.from(secp256k1.getPublicKey(priv, true)), sign: (hash: Buffer) => Buffer.from(secp256k1.sign(hash, priv, { lowS: true }).toCompactRawBytes()) };
    for (const i of parsed.ownerInputs) psbt.signInput(i.index, signer);
    return { ok: true, sig: new Uint8Array(psbt.toBuffer()) };
  }

  async setPin() { const ok = await this.waitTouch("pin", "pin", 60_000); if (ok) this.pinSet = true; return ok; }

  async elevate(limit: bigint) {
    const ok = await this.waitTouch(`e${Date.now()}`, "long_touch", 60_000);
    if (ok) { this.elevatedLimit = limit; this.elevatedUntil = Date.now() + 120_000; }
    return ok;
  }

  async allowance(a: Omit<Allowance, "spent"> | { revoke: true }) {
    if ("revoke" in a) { this.allow = null; return true; }
    const ok = await this.waitTouch(`a${Date.now()}`, "long_touch", 60_000);
    if (ok) this.allow = { ...a, spent: "0" };
    return ok;
  }

  async lock() { this.locked = true; this.allow = null; if (this.pend) { const p = this.pend; this.pend = null; clearTimeout(p.timer); p.resolve(false); } }
  /** The rhythm is checked against the real PIN and the duress PIN alike; the reply is identical either way. */
  async unlock() {
    const tapped = await this.waitTouch(`u${Date.now()}`, "pin", 60_000);
    if (!tapped) return false;
    const kind = this.nextRhythm; this.nextRhythm = "real";
    if (kind === "real") { this.locked = false; this.duressActive = false; return true; }
    if (kind === "duress" && this.duressPinSet) { this.locked = false; this.duressActive = true; this.allow = null; return true; }
    this.emit({ t: "event", ev: "pin_failed" });
    return false;
  }
  async setDuressPin(): Promise<{ ok: true } | { ok: false; reason: DenyReason }> {
    if (!this.initialized) return { ok: false, reason: "not_initialized" };
    if (!this.pinSet) return { ok: false, reason: "pin_not_set" };
    if (this.duressActive) return { ok: false, reason: "duress" };
    if (this.locked) return { ok: false, reason: "locked" };
    const ok = await this.waitTouch(`dp${Date.now()}`, "pin", 60_000);
    if (!ok) return { ok: false, reason: "timeout" };
    // the real controller rejects a rhythm identical to the real PIN; the test hook stands in for that comparison
    if (this.nextRhythm === "real") { this.nextRhythm = "real"; return { ok: false, reason: "pin_mismatch" }; }
    this.nextRhythm = "real";
    this.duressPinSet = true;
    return { ok: true };
  }
  async decoyAddresses(): Promise<{ ok: true; addresses: Addresses } | { ok: false; reason: DenyReason }> {
    if (!this.initialized) return { ok: false, reason: "not_initialized" };
    if (this.duressActive) return { ok: false, reason: "duress" };
    if (this.locked) return { ok: false, reason: "locked" };
    const ok = await this.waitTouch(`da${Date.now()}`, "pin", 60_000);
    if (!ok) return { ok: false, reason: "timeout" };
    return { ok: true, addresses: this.addressesOf(this.decoy) };
  }
  async duress() { this.duressActive = true; this.allow = null; }
  async setPolicy(dailyLimitLamports: bigint) { const ok = await this.waitTouch(`p${Date.now()}`, "long_touch", 60_000); if (ok) this.policy.daily_limit_lamports = dailyLimitLamports.toString(); return ok; }
  async setChainPolicy(chain: Exclude<PolicyChain, "solana">, limit: bigint) {
    const ok = await this.waitTouch(`p${Date.now()}`, "long_touch", 60_000);
    if (ok) { if (chain === "evm") this.policy.daily_limit_wei = limit.toString(); else this.policy.daily_limit_sats = limit.toString(); }
    return ok;
  }
  // ── firmware update: same checks as the firmware's update.c, in memory. `apply` "reboots" the sim: the
  // staged version becomes `fw`, the partition flips, and the image is on trial until `commit`. ──
  private fw = "sim-0.2.0";
  private partition = 0;
  private trial = false;
  private upd: { state: "receiving" | "verified"; size: number; sha256: string; version: string; buf: Buffer; received: number } | null = null;
  static readonly PARTITION_SIZE = 1024 * 1024;
  async updateBegin(size: number, sha256Hex: string, version: string): Promise<UpdateBegin> {
    if (this.upd?.state === "receiving") return { ok: false, reason: "state" };
    if (size <= 0 || size > SimController.PARTITION_SIZE || size % FW_PAGE) return { ok: false, reason: "size" };
    if (!version || version.length >= 16) return { ok: false, reason: "version" };
    this.upd = { state: "receiving", size, sha256: sha256Hex.toLowerCase(), version, buf: Buffer.alloc(size, 0xff), received: 0 };
    return { ok: true, chunk: FW_CHUNK, partition_size: SimController.PARTITION_SIZE };
  }
  async updateChunk(off: number, data: Uint8Array): Promise<UpdateResult> {
    const u = this.upd;
    if (!u || u.state !== "receiving") return { ok: false, reason: "state" };
    if (off !== u.received) return { ok: false, reason: "offset" };
    if (!data.length || data.length > FW_CHUNK || data.length % FW_PAGE || off + data.length > u.size) return { ok: false, reason: "length" };
    u.buf.set(data, off); u.received += data.length;
    return { ok: true };
  }
  async updateEnd(): Promise<UpdateResult> {
    const u = this.upd;
    if (!u || u.state !== "receiving") return { ok: false, reason: "state" };
    this.upd = null;
    if (u.received !== u.size) return { ok: false, reason: "size" };
    if (createHash("sha256").update(u.buf).digest("hex") !== u.sha256) return { ok: false, reason: "hash" };
    const c = checkImage(u.buf);
    if (!c.ok) return { ok: false, reason: "image" };
    this.upd = { ...u, state: "verified" };
    return { ok: true };
  }
  async updateApply(onPending?: PendingHook, ttlMs = 60_000): Promise<UpdateResult> {
    const u = this.upd;
    if (!u || u.state !== "verified") return { ok: false, reason: "state" };
    if (this.initialized && !this.pinSet) return { ok: false, reason: "pin_not_set" };
    if (this.initialized && this.locked) return { ok: false, reason: "locked" };
    if (this.duressActive) return { ok: false, reason: "duress" };
    onPending?.({ need: "long_touch", parsed: { version: u.version } });
    const ok = await this.waitTouch(`fw${Date.now()}`, "long_touch", ttlMs);
    if (!ok) return { ok: false, reason: "timeout" };
    this.upd = null; this.fw = u.version; this.partition ^= 1; this.trial = true;
    return { ok: true };
  }
  async updateCommit() { const was = this.trial; this.trial = false; return { ok: true as const, was_trial: was, bought: false }; }
  async updateAbort() { this.upd = null; }

  async close() {}
}

export { getAddress };
