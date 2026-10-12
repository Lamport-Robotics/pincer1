/**
 * Solana behind the chain-adapter interface. Pure delegation to tx/parse, tx/explain, risk/filter and
 * risk/intel: nothing about the Solana path changed when the daemon went multi-chain, the Signer just
 * reaches it through the same door as EVM and Bitcoin.
 */
import { Connection, PublicKey, VersionedTransaction } from "@solana/web3.js";
import { claimsFor, parseMessage, type ParsedTx } from "../../tx/parse.js";
import { explain, fmtSol, type Explanation } from "../../tx/explain.js";
import { assess, type RiskVerdict } from "../../risk/filter.js";
import { Intel, type AddressIntel } from "../../risk/intel.js";
import type { AssessCtx, ChainAdapter, ChainAddressIntel, ChainIntel, ChainParsed, ExplainCtx, ParseContext } from "../types.js";
import type { SolanaClaims } from "../../controller/types.js";
import { JUPITER_V6 } from "../../tx/known.js";

export interface SolanaParsed extends ChainParsed {
  chain: "solana";
  tx: ParsedTx;
  messageBytes: Uint8Array;
}

const toChainIntel = (i: AddressIntel): ChainAddressIntel => ({ address: i.address, exists: i.exists, isContract: i.isProgram, ageDays: i.ageDays, txCount: i.txCount, txCountIsLowerBound: i.txCountIsLowerBound, balance: BigInt(Math.round(i.lamports)), fetchedAt: i.fetchedAt });
const fromChainIntel = (i: ChainAddressIntel): AddressIntel => ({ address: i.address, exists: i.exists, isProgram: i.isContract, ageDays: i.ageDays, txCount: i.txCount, txCountIsLowerBound: i.txCountIsLowerBound, lamports: Number(i.balance ?? 0n), fetchedAt: i.fetchedAt });

class SolanaIntel implements ChainIntel {
  constructor(readonly inner: Intel) {}
  async address(a: string) { return toChainIntel(await this.inner.address(a)); }
  async token(m: string) { const r = await this.inner.mint(m).catch(() => null); return r && r.decimals != null ? { decimals: r.decimals } : null; }
  async ownerBalance(owner: string) { const b = await this.inner.ownerBalance(owner); return b >= 0 ? BigInt(b) : null; }
}

export class SolanaAdapter implements ChainAdapter<SolanaParsed> {
  readonly id = "solana" as const;
  readonly signKind = "tx" as const;
  readonly nativeSymbol = "SOL";
  readonly nativeDecimals = 9;
  readonly intel: SolanaIntel;
  constructor(readonly conn: Connection, intel?: Intel) { this.intel = new SolanaIntel(intel ?? new Intel(conn)); }
  intelFor(_parsed?: SolanaParsed) { return this.intel; }

  /** Resolve v0 address lookup tables through the RPC so the parse is complete. */
  private async lookupTables(bytes: Uint8Array): Promise<Record<string, string[]>> {
    const out: Record<string, string[]> = {};
    try {
      const tx = VersionedTransaction.deserialize(bytes);
      const msg = tx.message;
      if (msg.version === 0) {
        for (const l of msg.addressTableLookups) {
          const key = l.accountKey.toBase58();
          const r = await this.conn.getAddressLookupTable(new PublicKey(key)).catch(() => null);
          if (r?.value) out[key] = r.value.state.addresses.map((a) => a.toBase58());
        }
      }
    } catch { /* legacy or malformed: nothing to resolve */ }
    return out;
  }

  async parse(bytes: Uint8Array, ctx: ParseContext): Promise<SolanaParsed> {
    const tx = VersionedTransaction.deserialize(bytes);
    const messageBytes = tx.message.serialize();
    const tables = await this.lookupTables(bytes);
    const p = parseMessage(messageBytes, { owner: ctx.owner, lookupTables: tables });
    const counterparties = new Set<string>();
    if (p.dest) counterparties.add(p.dest);
    for (const t of p.tokenOut) counterparties.add(t.dest);
    for (const d of p.dangers) if (d.to) counterparties.add(d.to);
    for (const prog of p.programs) counterparties.add(prog);
    return {
      chain: "solana", tx: p, messageBytes,
      nativeOut: p.lamportsOut, nativeUnit: "lamports", dest: p.dest,
      tokenOut: p.tokenOut.map((t) => ({ token: t.mint, amount: t.amount, dest: t.dest })),
      dangers: p.dangers.map((d) => ({ kind: d.kind, detail: d.detail, to: d.to, amount: d.amount })),
      programs: p.programs, signers: p.signers,
      counterparties: [...counterparties].filter((a) => a !== "?" && a !== ctx.owner),
    };
  }

  signingBytes(_bytes: Uint8Array, parsed: SolanaParsed) { return parsed.messageBytes; }
  tokens(parsed: SolanaParsed): string[] { return parsed.tx.tokenOut.map((t) => t.mint).filter((m): m is string => !!m); }
  readonly routers = new Set<string>([JUPITER_V6]);

  explain(parsed: SolanaParsed, ctx: ExplainCtx): Explanation {
    const intel: Record<string, AddressIntel> = {};
    for (const [k, v] of Object.entries(ctx.intel)) intel[k] = fromChainIntel(v);
    return explain(parsed.tx, { origin: ctx.origin, intel, addressBook: ctx.addressBook, tokenMeta: ctx.tokenMeta });
  }

  assess(parsed: SolanaParsed, ctx: AssessCtx): RiskVerdict {
    const intel: Record<string, AddressIntel> = {};
    for (const [k, v] of Object.entries(ctx.intel)) intel[k] = fromChainIntel(v);
    return assess({ tx: parsed.tx, owner: ctx.owner, ownerLamports: ctx.ownerBalance != null ? Number(ctx.ownerBalance) : undefined, intel, addressBook: ctx.addressBook, origin: ctx.origin, originAllowlist: ctx.originAllowlist, dailyLimitLamports: ctx.dailyLimit, seenPrograms: ctx.seenPrograms });
  }

  claims(parsed: SolanaParsed): SolanaClaims { return claimsFor(parsed.tx); }

  async assemble(unsignedBytes: Uint8Array, sig: Uint8Array, _parsed: SolanaParsed, owner: string): Promise<Uint8Array> {
    const tx = VersionedTransaction.deserialize(unsignedBytes);
    tx.addSignature(new PublicKey(owner), sig);
    return tx.serialize();
  }

  async broadcast(bytes: Uint8Array): Promise<string> {
    return this.conn.sendRawTransaction(bytes, { skipPreflight: false, maxRetries: 3 });
  }

  fmtNative(n: bigint) { return fmtSol(n); }
}
