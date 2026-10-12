/**
 * Bitcoin chain adapter (native segwit, P2WPKH). UTXOs, fees and broadcast come from an Esplora-compatible
 * API; the PSBT is built here, signed by the controller, finalised here.
 */
import { secp256k1 } from "@noble/curves/secp256k1";
import type { Explanation } from "../../tx/explain.js";
import type { RiskVerdict } from "../../risk/filter.js";
import type { BtcClaims, BtcDanger } from "../../controller/types.js";
import type { AssessCtx, ChainAdapter, ChainAddressIntel, ChainIntel, ExplainCtx, ParseContext } from "../types.js";
import { Esplora } from "./esplora.js";
import { buildPsbt, finalisePsbt, parsePsbt, sameTransaction, type BtcNetwork, type BtcParsed } from "./psbt.js";
import { assessBtc, explainBtc, fmtBtc } from "./explain.js";

const TTL_MS = 5 * 60_000;

class BtcIntel implements ChainIntel {
  private cache = new Map<string, ChainAddressIntel>();
  constructor(private api: Esplora) {}
  async address(a: string): Promise<ChainAddressIntel> {
    const hit = this.cache.get(a);
    if (hit && Date.now() - hit.fetchedAt < TTL_MS) return hit;
    let out: ChainAddressIntel;
    try {
      const s = await this.api.stats(a);
      out = { address: a, exists: s.txCount > 0, isContract: false, ageDays: null, txCount: s.txCount, txCountIsLowerBound: false, balance: BigInt(s.balanceSats), fetchedAt: Date.now() };
    } catch {
      out = { address: a, exists: true, isContract: false, ageDays: null, txCount: 0, txCountIsLowerBound: true, balance: null, fetchedAt: Date.now() };
    }
    this.cache.set(a, out);
    return out;
  }
  async token() { return null; }
  async ownerBalance(owner: string): Promise<bigint | null> {
    try { return BigInt((await this.api.stats(owner)).balanceSats); } catch { return null; }
  }
  seed(entries: Partial<ChainAddressIntel>[]) { for (const e of entries) this.cache.set(e.address!, { exists: true, isContract: false, ageDays: null, txCount: 0, txCountIsLowerBound: false, balance: 0n, fetchedAt: Date.now(), ...e } as ChainAddressIntel); }
}

export interface BitcoinAdapterOptions { esplora: Esplora; network?: BtcNetwork; feeTargetBlocks?: number }

export class BitcoinAdapter implements ChainAdapter<BtcParsed> {
  readonly id = "bitcoin" as const;
  readonly signKind = "btc_psbt" as const;
  readonly nativeSymbol = "BTC";
  readonly nativeDecimals = 8;
  readonly network: BtcNetwork;
  readonly intel: BtcIntel;
  constructor(readonly opts: BitcoinAdapterOptions) { this.network = opts.network ?? "mainnet"; this.intel = new BtcIntel(opts.esplora); }
  intelFor() { return this.intel; }

  async parse(bytes: Uint8Array, ctx: ParseContext): Promise<BtcParsed> { return parsePsbt(bytes, ctx.owner, this.network); }
  signingBytes(bytes: Uint8Array) { return bytes; }
  explain(p: BtcParsed, ctx: ExplainCtx): Explanation { return explainBtc(p, ctx); }
  assess(p: BtcParsed, ctx: AssessCtx): RiskVerdict { return assessBtc(p, ctx); }

  claims(p: BtcParsed): BtcClaims {
    const dangerous = new Set<BtcDanger>();
    for (const d of p.dangers) dangerous.add(d.kind as BtcDanger);
    if (p.feeSats >= 0n && p.satsOut > 0n && Number(p.feeSats) / Number(p.satsOut) > 0.05) dangerous.add("high_fee");
    return { network: this.network, sats_out: p.satsOut.toString(), dest: p.dest ?? "", fee_sats: p.feeSats >= 0n ? p.feeSats.toString() : "0", inputs: p.ownerInputs.length, dangerous: [...dangerous] };
  }

  /** The controller's output is the PSBT with partial signatures; verify every one, finalise, return the raw tx. */
  async assemble(unsignedBytes: Uint8Array, signedPsbt: Uint8Array): Promise<Uint8Array> {
    if (!sameTransaction(unsignedBytes, signedPsbt, this.network)) throw new Error("controller returned a different transaction");
    const fin = finalisePsbt(signedPsbt, this.network, (pubkey, msghash, signature) => { try { return secp256k1.verify(signature, msghash, pubkey); } catch { return false; } });
    return new Uint8Array(Buffer.from(fin.txHex, "hex"));
  }

  async broadcast(bytes: Uint8Array): Promise<string> { return this.opts.esplora.broadcast(Buffer.from(bytes).toString("hex")); }
  fmtNative(n: bigint) { return fmtBtc(n); }

  /** Build a payment PSBT for the owner (used by the owner-facing send and by the swap module). */
  async buildPayment(owner: string, dest: string, amountSats: bigint, memo?: string) {
    const [utxos, fee] = await Promise.all([this.opts.esplora.utxos(owner), this.opts.esplora.feeRate(this.opts.feeTargetBlocks ?? 3)]);
    const r = buildPsbt({ network: this.network, owner, dest, amountSats, utxos, feeRate: fee.rate, memo });
    return { psbt: new Uint8Array(r.psbt.toBuffer()), feeSats: r.feeSats, changeSats: r.changeSats, feeRate: fee.rate, feeClamped: fee.clamped, vsize: r.vsize };
  }
}
