/**
 * Swaps, owner-facing. Online mode only.
 *
 *   POST /swap/quote   → a quote the robot can speak, pinned for ten minutes
 *   POST /swap/execute → router builds the bytes → verify.ts re-decodes them (router allowlist, min output,
 *                        slippage cap, exact-amount approval) → the normal Signer pipeline (presence,
 *                        explain, touch, controller re-check) → broadcast
 *
 * Nothing a router returns is trusted. The owner's protection is the independent decode plus the
 * controller's own parse; the routers only provide liquidity.
 */
import { getAddress, type Address } from "viem";
import { randomBytes } from "node:crypto";
import type { Signer } from "../sign.js";
import type { Settings } from "../settings.js";
import type { ChainId } from "../chains/types.js";
import type { SolanaParsed } from "../chains/solana/index.js";
import type { EvmAdapter } from "../chains/evm/index.js";
import type { EvmParsed } from "../chains/evm/parse.js";
import type { BitcoinAdapter } from "../chains/bitcoin/index.js";
import type { BtcParsed } from "../chains/bitcoin/psbt.js";
import { fmtNativeEvm, fmtEvmToken } from "../chains/evm/explain.js";
import { fmtBtc } from "../chains/bitcoin/explain.js";
import { fmtSol, fmtToken } from "../tx/explain.js";
import { KNOWN_MINTS } from "../tx/known.js";
import { CHAIN_NAMES, NATIVE_SYMBOL } from "../chains/evm/rpc.js";
import { JupiterProvider, OneInchProvider, ThorchainProvider, ZeroXProvider, approveCalldata, buildEvmTx, type EvmAggregator } from "./providers.js";
import { DEFAULT_THORNODES, ROUTER_NAMES, ThorPins } from "./routers.js";
import { NATIVE_SENTINEL, SwapVerifyError, decodeOneInch, decodeThorDeposit, decodeZeroX, fromThorUnits, parseThorMemo, toThorUnits, verifyJupiterTx } from "./verify.js";

export class SwapError extends Error { constructor(readonly code: string, message: string) { super(message); } }

export interface SwapAsset { chain: ChainId; asset: string; chainId?: number }
export interface SwapQuoteRequest { from: SwapAsset; to: SwapAsset; amount: string | number; slippageBps?: number }

interface Leg { symbol: string; decimals: number; native: boolean; id: string; chainId?: number }

export interface SwapQuote {
  id: string;
  route: "jupiter" | "1inch" | "0x" | "thorchain";
  from: Leg & { chain: ChainId }; to: Leg & { chain: ChainId };
  amountIn: bigint; expectedOut: bigint; minOut: bigint; slippageBps: number;
  spoken: string;
  createdAt: number; expiresAt: number;
  /** Provider payload needed at execution (Jupiter quoteResponse, THORChain quote). */
  raw?: unknown;
}

export interface SwapExecuteResult { ok: true; quoteId: string; steps: { kind: "approve" | "swap"; chain: ChainId; txid: string }[]; spoken: string }

export interface SwapDeps {
  signer: Signer;
  settings: () => Pick<Settings, "mode" | "swapMaxSlippageBps">;
  evm?: EvmAdapter;
  bitcoin?: BitcoinAdapter;
  jupiter?: JupiterProvider;
  aggregators?: EvmAggregator[];
  thorchain?: ThorchainProvider;
  thorPins?: ThorPins;
  fetchImpl?: typeof fetch;
  now?: () => number;
  /** Hard cap in basis points; settings may lower it, never raise it. */
  maxSlippageBps?: number;
}

const QUOTE_TTL_MS = 10 * 60_000;
const SOL = "So11111111111111111111111111111111111111112";
const fmtAmt = (n: bigint, leg: Leg) => {
  const v = Number(n) / 10 ** leg.decimals;
  return `${v.toLocaleString("en-US", { maximumFractionDigits: v < 1 ? 6 : 2 })} ${leg.symbol}`;
};
const parseHuman = (amount: string | number, decimals: number): bigint => {
  const s = String(amount).trim();
  if (!/^\d+(\.\d+)?$/.test(s)) throw new SwapError("bad_amount", `amount "${s}" is not a plain decimal number`);
  const [i, f = ""] = s.split(".");
  if (f.length > decimals) throw new SwapError("bad_amount", `amount has more decimals than the asset (${decimals})`);
  const v = BigInt(i) * 10n ** BigInt(decimals) + BigInt((f + "0".repeat(decimals)).slice(0, decimals) || "0");
  if (v <= 0n) throw new SwapError("bad_amount", "amount must be positive");
  return v;
};

export class SwapService {
  private quotes = new Map<string, SwapQuote>();
  private now: () => number;
  readonly thorPins: ThorPins;
  private jupiter: JupiterProvider;
  private aggregators: EvmAggregator[];
  private thorchain: ThorchainProvider;
  constructor(private d: SwapDeps) {
    this.now = d.now ?? Date.now;
    this.jupiter = d.jupiter ?? new JupiterProvider(undefined, d.fetchImpl);
    this.aggregators = d.aggregators ?? [new OneInchProvider(process.env.ONEINCH_API_KEY ?? "", undefined, d.fetchImpl), new ZeroXProvider(process.env.ZEROX_API_KEY ?? "", undefined, d.fetchImpl)];
    this.thorchain = d.thorchain ?? new ThorchainProvider(undefined, d.fetchImpl);
    this.thorPins = d.thorPins ?? new ThorPins([process.env.THORNODE_URL ?? DEFAULT_THORNODES[0], process.env.THORNODE_URL_2 ?? DEFAULT_THORNODES[1]], d.fetchImpl, this.now, (router) => d.evm?.routers.add(router.toLowerCase()));
  }

  private requireOnline() {
    if (this.d.settings().mode !== "online") throw new SwapError("offline_mode", "swaps need the network and the wallet is in offline mode. Switch mode to online in settings first.");
  }
  private slippage(requested?: number): number {
    const cap = Math.min(this.d.maxSlippageBps ?? 100, this.d.settings().swapMaxSlippageBps ?? 100, 100);
    if (requested == null) return cap;
    if (!Number.isInteger(requested) || requested < 1) throw new SwapError("bad_slippage", "slippageBps must be a positive integer");
    return Math.min(requested, cap);
  }
  private minOut(expected: bigint, slippageBps: number) { return (expected * BigInt(10_000 - slippageBps)) / 10_000n; }

  // ── asset resolution ──
  private async leg(a: SwapAsset): Promise<Leg & { chain: ChainId }> {
    const asset = a.asset.trim();
    if (a.chain === "solana") {
      if (/^sol$/i.test(asset) || asset === SOL) return { chain: "solana", symbol: "SOL", decimals: 9, native: true, id: SOL };
      const known = KNOWN_MINTS[asset];
      if (known) return { chain: "solana", symbol: known.symbol, decimals: known.decimals, native: false, id: asset };
      const m = await this.d.signer.intel.mint(asset).catch(() => null);
      if (m?.decimals == null) throw new SwapError("unknown_asset", `I don't know the token ${asset}`);
      return { chain: "solana", symbol: `token ${asset.slice(0, 4)}…${asset.slice(-4)}`, decimals: m.decimals, native: false, id: asset };
    }
    if (a.chain === "evm") {
      const chainId = a.chainId ?? 1;
      if (!this.d.evm?.hasChain(chainId)) throw new SwapError("unknown_chain", `no RPC configured for chain ${chainId}`);
      const sym = NATIVE_SYMBOL[chainId] ?? "ETH";
      if (/^(eth|bnb|pol|matic|native)$/i.test(asset) || asset.toLowerCase() === NATIVE_SENTINEL.toLowerCase()) return { chain: "evm", chainId, symbol: sym, decimals: 18, native: true, id: NATIVE_SENTINEL };
      const token = getAddress(asset);
      const meta = await this.d.evm.intelForChain(chainId).token(token);
      if (meta?.decimals == null) throw new SwapError("unknown_asset", `I can't read the decimals of ${asset} on ${CHAIN_NAMES[chainId] ?? chainId}`);
      return { chain: "evm", chainId, symbol: meta.symbol ?? fmtEvmToken(null, token, undefined, chainId), decimals: meta.decimals, native: false, id: token };
    }
    if (!/^btc$/i.test(asset)) throw new SwapError("unknown_asset", "on Bitcoin the only asset is BTC");
    if (!this.d.bitcoin) throw new SwapError("unknown_chain", "Bitcoin is not configured");
    return { chain: "bitcoin", symbol: "BTC", decimals: 8, native: true, id: "BTC" };
  }

  private thorAsset(l: Leg & { chain: ChainId }): string {
    if (l.chain === "bitcoin") return "BTC.BTC";
    if (l.chain === "evm") {
      const prefix: Record<number, string> = { 1: "ETH", 8453: "BASE", 42161: "ARB", 56: "BSC", 137: "POL" }; // THORChain chain tags
      const tag = prefix[l.chainId ?? 1];
      if (!tag) throw new SwapError("unsupported", `THORChain does not serve chain ${l.chainId}`);
      return l.native ? `${tag}.${l.symbol}` : `${tag}.${l.symbol}-${l.id.toUpperCase()}`;
    }
    throw new SwapError("unsupported", "THORChain route from Solana is not supported by this wallet");
  }

  // ── quote ──
  async quote(req: SwapQuoteRequest): Promise<SwapQuote> {
    this.requireOnline();
    const [from, to] = await Promise.all([this.leg(req.from), this.leg(req.to)]);
    const slippageBps = this.slippage(req.slippageBps);
    const amountIn = parseHuman(req.amount, from.decimals);
    let route: SwapQuote["route"]; let expectedOut: bigint; let raw: unknown; let via: string;
    if (from.chain === "solana" && to.chain === "solana") {
      const q = await this.jupiter.quote(from.id, to.id, amountIn, slippageBps);
      route = "jupiter"; expectedOut = q.outAmount; raw = q.raw; via = "Jupiter";
    } else if (from.chain === "evm" && to.chain === "evm") {
      if (from.chainId !== to.chainId) throw new SwapError("unsupported", "both EVM assets must be on the same chain; use a THORChain route for cross-chain");
      const candidates = this.aggregators.filter((a) => a.supports(from.chainId!));
      if (!candidates.length) throw new SwapError("no_provider", "no EVM aggregator is configured for this chain (set ONEINCH_API_KEY or ZEROX_API_KEY)");
      // 1inch first, 0x as the fallback when 1inch cannot quote
      let picked: EvmAggregator | null = null; let out = 0n; let lastErr: unknown = null;
      for (const agg of candidates) { try { out = (await agg.quote(from.chainId!, from.id, to.id, amountIn)).outAmount; picked = agg; break; } catch (e) { lastErr = e; } }
      if (!picked) throw lastErr instanceof Error ? lastErr : new SwapError("no_quote", String(lastErr));
      route = picked.name; via = picked.name; expectedOut = out; raw = { aggregator: picked.name };
    } else if (from.chain !== "solana" && to.chain !== "solana") {
      const owner = this.d.signer.ownerFor(to.chain);
      if (!owner) throw new SwapError("no_key", `the controller has no ${to.chain} key to receive on`);
      if (from.chain === "evm" && !from.native) throw new SwapError("unsupported", "cross-chain swaps from ERC-20 tokens are not supported; swap to the native coin first");
      const q = await this.thorchain.quote(this.thorAsset(from), this.thorAsset(to), toThorUnits(amountIn, from.decimals), owner, slippageBps);
      if (q.recommendedMinIn > 0n && toThorUnits(amountIn, from.decimals) < q.recommendedMinIn) throw new SwapError("too_small", `THORChain needs at least ${fmtAmt(fromThorUnits(q.recommendedMinIn, from.decimals), from)} for this route`);
      route = "thorchain"; expectedOut = fromThorUnits(q.expectedOut, to.decimals); raw = q; via = "THORChain";
    } else throw new SwapError("unsupported", "Solana cannot be bridged with this wallet; THORChain routes cover Bitcoin and EVM chains");

    const minOut = this.minOut(expectedOut, slippageBps);
    const where = from.chain === "evm" ? ` on ${CHAIN_NAMES[from.chainId!] ?? "chain " + from.chainId}` : "";
    const cross = from.chain !== to.chain ? ` to ${to.chain === "bitcoin" ? "Bitcoin" : to.chain === "evm" ? CHAIN_NAMES[to.chainId!] ?? "chain " + to.chainId : "Solana"}` : "";
    const spoken = `Swapping ${fmtAmt(amountIn, from)}${where} for about ${fmtAmt(expectedOut, to)}${cross} through ${via}. You'll get at least ${fmtAmt(minOut, to)}, that's ${slippageBps / 100}% slippage. Say execute within ten minutes.`;
    const id = randomBytes(6).toString("hex");
    const quote: SwapQuote = { id, route, from, to, amountIn, expectedOut, minOut, slippageBps, spoken, createdAt: this.now(), expiresAt: this.now() + QUOTE_TTL_MS, raw };
    this.quotes.set(id, quote);
    for (const [k, v] of this.quotes) if (v.expiresAt < this.now()) this.quotes.delete(k);
    return quote;
  }

  get(id: string) { return this.quotes.get(id) ?? null; }

  // ── execute ──
  async execute(quoteId: string, origin?: string): Promise<SwapExecuteResult> {
    this.requireOnline();
    const q = this.quotes.get(quoteId);
    if (!q) throw new SwapError("unknown_quote", "I don't have that quote; ask for a new one");
    if (q.expiresAt < this.now()) { this.quotes.delete(quoteId); throw new SwapError("expired", "that quote is older than ten minutes; ask for a new one"); }
    this.quotes.delete(quoteId); // one execution per quote
    const preface = `You asked to swap ${fmtAmt(q.amountIn, q.from)} for at least ${fmtAmt(q.minOut, q.to)}.`;
    switch (q.route) {
      case "jupiter": return this.executeJupiter(q, preface, origin);
      case "1inch": case "0x": return this.executeEvm(q, preface, origin);
      case "thorchain": return this.executeThor(q, preface, origin);
    }
  }

  private async signAndSend<P extends SolanaParsed | EvmParsed | BtcParsed>(chain: ChainId, bytes: Uint8Array, preface: string, labels: Record<string, string>, origin?: string): Promise<{ txid: string; parsed: P }> {
    const r = await this.d.signer.signChain<P>({ chain, bytes, source: "api", preface, labels, origin });
    if (!r.ok) throw new SwapError(r.reason, r.detail);
    const txid = await this.d.signer.broadcast(chain, r.signedTx, r.parsed);
    return { txid, parsed: r.parsed };
  }

  private async executeJupiter(q: SwapQuote, preface: string, origin?: string): Promise<SwapExecuteResult> {
    const owner = this.d.signer.getOwner();
    const bytes = await this.jupiter.build(q.raw, owner);
    const ex = await this.d.signer.explainChain<SolanaParsed>("solana", bytes, origin);
    try { verifyJupiterTx(ex.parsed.tx, q.minOut, q.slippageBps, q.amountIn, { owner, inMint: q.from.id, outMint: q.to.id }); } catch (e) { throw this.verifyFail(e); }
    if (ex.parsed.tx.lamportsOut > (q.from.native ? q.amountIn + 20_000_000n : 20_000_000n)) throw new SwapError("amount", "the swap transaction moves more SOL than quoted");
    const { txid } = await this.signAndSend<SolanaParsed>("solana", bytes, preface, {}, origin);
    return { ok: true, quoteId: q.id, steps: [{ kind: "swap", chain: "solana", txid }], spoken: `Swap sent through Jupiter. At least ${fmtAmt(q.minOut, q.to)} is on its way.` };
  }

  private async executeEvm(q: SwapQuote, preface: string, origin?: string): Promise<SwapExecuteResult> {
    const evm = this.d.evm;
    const chainId = q.from.chainId!;
    const owner = this.d.signer.ownerFor("evm");
    if (!evm || !owner) throw new SwapError("no_key", "the controller has no evm key");
    const agg = this.aggregators.find((a) => a.name === q.route);
    if (!agg) throw new SwapError("no_provider", `${q.route} is not configured`);
    const from = getAddress(owner);
    const built = await agg.build(chainId, q.from.id, q.to.id, q.amountIn, from, q.slippageBps);
    // ── independent verification of the router's bytes ──
    const routerL = agg.router.toLowerCase();
    if (built.tx.to.toLowerCase() !== routerL) throw new SwapError("router", `the swap goes to ${built.tx.to}, not the ${agg.name} router`);
    if (!evm.routers.has(routerL)) throw new SwapError("router", `${agg.name}'s router is not on the allowlist`);
    const dec = agg.name === "1inch" ? (() => { try { return decodeOneInch(built.tx.data, built.tx.value); } catch (e) { throw this.verifyFail(e); } })() : (() => { try { return decodeZeroX(built.tx.data, built.tx.value); } catch (e) { throw this.verifyFail(e); } })();
    if (dec.minOut < q.minOut) throw new SwapError("min_out", `the router only guarantees ${fmtAmt(dec.minOut, q.to)} but the quote promised ${fmtAmt(q.minOut, q.to)}`);
    if (dec.amountIn != null && dec.amountIn > q.amountIn) throw new SwapError("amount", "the router wants to spend more than quoted");
    if (q.from.native) { if (built.tx.value > q.amountIn) throw new SwapError("amount", "the router asks for more native value than quoted"); if (dec.srcToken) throw new SwapError("amount", "the router swaps a token but a native swap was quoted"); }
    else { if (built.tx.value !== 0n) throw new SwapError("amount", "a token swap must not send native value"); if (dec.srcToken && dec.srcToken.toLowerCase() !== q.from.id.toLowerCase()) throw new SwapError("amount", "the router swaps a different token than quoted"); }
    if (dec.receiver && dec.receiver.toLowerCase() !== from.toLowerCase()) throw new SwapError("receiver", `the swap output would go to ${dec.receiver}, not to you`);
    // the output token must be the one quoted: a router that pays out in a worthless token still "guarantees" a minimum
    if (q.to.native) { if (dec.dstToken) throw new SwapError("amount", "the router pays out a token but a native swap was quoted"); }
    else if (dec.dstToken && dec.dstToken.toLowerCase() !== q.to.id.toLowerCase()) throw new SwapError("amount", "the router pays out a different token than quoted");

    const rpc = evm.rpc(chainId);
    const intel = evm.intelForChain(chainId);
    let nonce = await rpc.transactionCount(from);
    const steps: SwapExecuteResult["steps"] = [];
    const labels = { [agg.router.toLowerCase()]: ROUTER_NAMES[routerL] ?? agg.name, [agg.router]: ROUTER_NAMES[routerL] ?? agg.name };
    // ── exact-amount approval, to the router only, when the allowance is short ──
    if (!q.from.native) {
      const allowance = (await intel.allowance(q.from.id, from, agg.router)) ?? 0n;
      if (allowance < q.amountIn) {
        const approveBytes = await buildEvmTx(rpc, { chainId, from, to: q.from.id as Address, data: approveCalldata(agg.router, q.amountIn), value: 0n, gas: 80_000n, gasPrice: built.tx.gasPrice, nonce });
        const ex = await this.d.signer.explainChain<EvmParsed>("evm", approveBytes, origin, chainId, labels);
        const c = ex.parsed.call;
        if (c.fn !== "approve" || c.spender.toLowerCase() !== routerL || c.amount !== q.amountIn || c.unlimited) throw new SwapError("approval", "the approval I built is not the exact-amount approval to the router that I expected");
        const { txid } = await this.signAndSend<EvmParsed>("evm", approveBytes, `${preface} First, an exact approval of ${fmtAmt(q.amountIn, q.from)} for ${agg.name}.`, labels, origin);
        steps.push({ kind: "approve", chain: "evm", txid });
        nonce += 1;
      }
    }
    const swapBytes = await buildEvmTx(rpc, { chainId, from, to: built.tx.to, data: built.tx.data, value: built.tx.value, gas: built.tx.gas != null ? (built.tx.gas * 12n) / 10n : null, gasPrice: built.tx.gasPrice, nonce });
    const ex = await this.d.signer.explainChain<EvmParsed>("evm", swapBytes, origin, chainId, labels);
    if ((ex.parsed.to ?? "").toLowerCase() !== routerL || ex.parsed.call.fn !== "router") throw new SwapError("router", "the built transaction does not call the allowlisted router");
    const { txid } = await this.signAndSend<EvmParsed>("evm", swapBytes, preface, labels, origin);
    steps.push({ kind: "swap", chain: "evm", txid });
    return { ok: true, quoteId: q.id, steps, spoken: `Swap sent through ${agg.name}. At least ${fmtAmt(q.minOut, q.to)} is on its way.` };
  }

  private async executeThor(q: SwapQuote, preface: string, origin?: string): Promise<SwapExecuteResult> {
    const tq = q.raw as import("./providers.js").ThorQuote;
    const owner = this.d.signer.ownerFor(q.to.chain);
    if (!owner) throw new SwapError("no_key", `the controller has no ${q.to.chain} key`);
    const limit = toThorUnits(q.minOut, q.to.decimals);
    const memo = `=:${this.thorAsset(q.to)}:${owner}:${limit}`;
    // the pinned inbound set is the allowlist; refresh if stale, then the address must be pinned
    const chainTag = q.from.chain === "bitcoin" ? "BTC" : ({ 1: "ETH", 8453: "BASE", 42161: "ARB", 56: "BSC", 137: "POL" } as Record<number, string>)[q.from.chainId ?? 1];
    const inbound = await this.thorPins.inbound(chainTag);
    if (inbound.address.toLowerCase() !== tq.inboundAddress.toLowerCase()) throw new SwapError("router", "THORChain's inbound address changed since the quote; ask for a new quote");
    const memoCheck = parseThorMemo(memo);
    if (memoCheck.limit == null || memoCheck.limit < limit) throw new SwapError("min_out", "memo limit below the quoted minimum");

    if (q.from.chain === "bitcoin") {
      const btc = this.d.bitcoin;
      const btcOwner = this.d.signer.ownerFor("bitcoin");
      if (!btc || !btcOwner) throw new SwapError("no_key", "the controller has no bitcoin key");
      if (tq.dustThreshold > 0n && q.amountIn <= tq.dustThreshold) throw new SwapError("too_small", "amount is at or below THORChain's dust threshold");
      const built = await btc.buildPayment(btcOwner, inbound.address, q.amountIn, memo);
      const labels = { [inbound.address]: "THORChain vault" };
      const ex = await this.d.signer.explainChain<BtcParsed>("bitcoin", built.psbt, origin, undefined, labels);
      if (ex.parsed.dest !== inbound.address || !this.thorPins.isPinned(ex.parsed.dest ?? "", "BTC")) throw new SwapError("router", "the payment does not go to the pinned THORChain vault");
      if (ex.parsed.satsOut !== q.amountIn) throw new SwapError("amount", "the payment amount differs from the quote");
      const m = parseThorMemo(ex.parsed.memo ?? "");
      if (m.limit == null || m.limit < limit || m.dest.toLowerCase() !== owner.toLowerCase()) throw new SwapError("min_out", "the memo in the transaction does not carry the quoted minimum to your address");
      const { txid } = await this.signAndSend<BtcParsed>("bitcoin", built.psbt, preface, labels, origin);
      return { ok: true, quoteId: q.id, steps: [{ kind: "swap", chain: "bitcoin", txid }], spoken: `Sent ${fmtBtc(q.amountIn)} to THORChain. At least ${fmtAmt(q.minOut, q.to)} will arrive on ${q.to.chain === "evm" ? CHAIN_NAMES[q.to.chainId!] ?? "the other chain" : "Bitcoin"}.` };
    }
    // EVM native → THORChain router depositWithExpiry(vault, 0x0, amount, memo, expiry)
    const evm = this.d.evm;
    const from = this.d.signer.ownerFor("evm");
    if (!evm || !from) throw new SwapError("no_key", "the controller has no evm key");
    if (!inbound.router) throw new SwapError("router", "THORChain has no router for this chain");
    if (!this.thorPins.isPinned(inbound.router, chainTag)) throw new SwapError("router", "THORChain router is not pinned");
    const chainId = q.from.chainId!;
    const { encodeFunctionData } = await import("viem");
    const { THOR_ROUTER_ABI } = await import("./verify.js");
    const expiry = BigInt(Math.floor(this.now() / 1000) + 15 * 60);
    const data = encodeFunctionData({ abi: THOR_ROUTER_ABI, functionName: "depositWithExpiry", args: [getAddress(inbound.address), "0x0000000000000000000000000000000000000000", q.amountIn, memo, expiry] });
    const rpc = evm.rpc(chainId);
    const nonce = await rpc.transactionCount(getAddress(from));
    const bytes = await buildEvmTx(rpc, { chainId, from: getAddress(from), to: getAddress(inbound.router), data, value: q.amountIn, gas: null, gasPrice: null, nonce });
    const labels = { [inbound.router.toLowerCase()]: "THORChain router", [inbound.router]: "THORChain router" };
    const ex = await this.d.signer.explainChain<EvmParsed>("evm", bytes, origin, chainId, labels);
    const dep = (() => { try { return decodeThorDeposit(ex.parsed.data); } catch (e) { throw this.verifyFail(e); } })();
    if ((ex.parsed.to ?? "").toLowerCase() !== inbound.router.toLowerCase()) throw new SwapError("router", "the transaction does not call the pinned THORChain router");
    if (dep.vault.toLowerCase() !== inbound.address.toLowerCase()) throw new SwapError("router", "the deposit vault is not the pinned inbound address");
    if (dep.amount !== q.amountIn || ex.parsed.value !== q.amountIn) throw new SwapError("amount", "deposit amount differs from the quote");
    const m = parseThorMemo(dep.memo);
    if (m.limit == null || m.limit < limit || m.dest.toLowerCase() !== owner.toLowerCase()) throw new SwapError("min_out", "the memo does not carry the quoted minimum to your address");
    const { txid } = await this.signAndSend<EvmParsed>("evm", bytes, preface, labels, origin);
    return { ok: true, quoteId: q.id, steps: [{ kind: "swap", chain: "evm", txid }], spoken: `Sent ${fmtNativeEvm(q.amountIn, chainId)} to THORChain. At least ${fmtAmt(q.minOut, q.to)} will arrive on ${q.to.chain === "bitcoin" ? "Bitcoin" : CHAIN_NAMES[q.to.chainId!] ?? "the other chain"}.` };
  }

  private verifyFail(e: unknown): SwapError {
    if (e instanceof SwapVerifyError) return new SwapError(e.code, e.message);
    if (e instanceof SwapError) return e;
    return new SwapError("verify", String((e as Error).message ?? e));
  }
}

export { fmtSol, fmtToken };
