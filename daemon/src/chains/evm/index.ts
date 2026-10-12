/**
 * EVM chain adapter: Ethereum, Base, Arbitrum, BNB Chain, Polygon (any chain id with an RPC configured).
 * Parsing and explanation are pure; intel and broadcast go through the per-chain RPC.
 */
import { getAddress, keccak256, parseTransaction, recoverAddress, serializeTransaction, type Hex } from "viem";
import type { Explanation } from "../../tx/explain.js";
import type { RiskVerdict } from "../../risk/filter.js";
import type { EvmClaims, EvmDanger } from "../../controller/types.js";
import type { AssessCtx, ChainAdapter, ChainIntel, ExplainCtx, ParseContext } from "../types.js";
import { EvmIntel, type EvmIntelOptions } from "./intel.js";
import { parseEvmTx, type EvmParsed } from "./parse.js";
import { explainEvm, fmtNativeEvm } from "./explain.js";
import { assessEvm } from "./risk.js";
import type { EvmRpc } from "./rpc.js";
import { EVM_ROUTERS } from "../../swap/routers.js";

export interface EvmAdapterOptions {
  rpcs: Map<number, EvmRpc>;
  intel?: EvmIntelOptions;
  /** Extra router addresses the owner trusts (lowercased on entry). */
  extraRouters?: string[];
}

export class EvmAdapter implements ChainAdapter<EvmParsed> {
  readonly id = "evm" as const;
  readonly signKind = "evm_tx" as const;
  readonly nativeSymbol = "ETH";
  readonly nativeDecimals = 18;
  private intels = new Map<number, EvmIntel>();
  /** Lowercased allowlisted router addresses (static list + live THORChain pins added by the swap module). */
  readonly routers = new Set<string>();
  constructor(readonly opts: EvmAdapterOptions) {
    for (const r of EVM_ROUTERS) this.routers.add(r.toLowerCase());
    for (const r of opts.extraRouters ?? []) this.routers.add(r.toLowerCase());
  }

  rpc(chainId: number): EvmRpc {
    const r = this.opts.rpcs.get(chainId);
    if (!r) throw new Error(`no RPC configured for chain ${chainId} (set EVM_RPC_${chainId})`);
    return r;
  }
  hasChain(chainId: number) { return this.opts.rpcs.has(chainId); }
  intelForChain(chainId: number): EvmIntel {
    let i = this.intels.get(chainId);
    if (!i) { i = new EvmIntel(this.rpc(chainId), this.opts.intel); this.intels.set(chainId, i); }
    return i;
  }
  intelFor(parsed: EvmParsed): ChainIntel { return this.intelForChain(parsed.chainId); }

  async parse(bytes: Uint8Array, ctx: ParseContext): Promise<EvmParsed> {
    const p = parseEvmTx(bytes, ctx, { routers: this.routers });
    if (!this.hasChain(p.chainId)) throw new Error(`chain ${p.chainId} is not configured (set EVM_RPC_${p.chainId})`);
    return p;
  }

  signingBytes(bytes: Uint8Array) { return bytes; }
  tokens(p: EvmParsed): string[] {
    const out = new Set<string>(p.tokenOut.map((t) => t.token).filter((t): t is string => !!t));
    const c = p.call as { token?: string };
    if (c.token) out.add(c.token);
    return [...out];
  }
  explain(p: EvmParsed, ctx: ExplainCtx): Explanation { return explainEvm(p, ctx); }
  assess(p: EvmParsed, ctx: AssessCtx): RiskVerdict { return assessEvm(p, { ...ctx, routers: new Set([...ctx.routers, ...this.routers]) }); }

  claims(p: EvmParsed): EvmClaims {
    const dangerous = new Set<EvmDanger>();
    for (const d of p.dangers) dangerous.add(d.kind as EvmDanger);
    // The controller only knows the built-in routers plus what the owner added with the PIN. A router this
    // daemon learned at runtime (THORChain's rotating router) is an unknown contract to the controller, so
    // declare it as such; the owner sees "unknown contract" on the trusted display and decides.
    if (p.call.fn === "router") { const router = p.call.router.toLowerCase(); if (!EVM_ROUTERS.some((r) => r.toLowerCase() === router)) dangerous.add("unknown_contract"); }
    return {
      chain_id: p.chainId,
      to: p.to ?? "",
      value_wei: p.value.toString(),
      token_out: p.tokenOut.map((t) => ({ token: t.token ?? "", amount: t.amount.toString(), to: t.dest ?? "" })),
      max_fee_wei: p.maxFeeWei.toString(),
      routers: [...this.routers],
      dangerous: [...dangerous],
    };
  }

  /** 65-byte r‖s‖v from the controller (v = 27/28 for legacy, 0/1 for typed) → signed RLP. The recovered signer must be the owner. */
  async assemble(unsignedBytes: Uint8Array, sig: Uint8Array, p: EvmParsed, owner: string): Promise<Uint8Array> {
    if (sig.length !== 65) throw new Error(`controller returned ${sig.length} bytes, expected 65`);
    const r = ("0x" + Buffer.from(sig.subarray(0, 32)).toString("hex")) as Hex;
    const s = ("0x" + Buffer.from(sig.subarray(32, 64)).toString("hex")) as Hex;
    const v = sig[64];
    const yParity = v >= 27 ? v - 27 : v;
    if (yParity !== 0 && yParity !== 1) throw new Error(`bad recovery id ${v}`);
    const digest = keccak256(("0x" + Buffer.from(unsignedBytes).toString("hex")) as Hex);
    // never hand back a signature that does not recover to the owner's address
    const who = await recoverAddress({ hash: digest, signature: { r, s, yParity } });
    if (who.toLowerCase() !== getAddress(owner).toLowerCase()) throw new Error("signature does not recover to the owner's address");
    const signed = serializeTransaction(p.tx, { r, s, v: BigInt(27 + yParity), yParity });
    return new Uint8Array(Buffer.from(signed.slice(2), "hex"));
  }

  async broadcast(bytes: Uint8Array, parsed?: EvmParsed): Promise<string> {
    const hex = ("0x" + Buffer.from(bytes).toString("hex")) as Hex;
    const chainId = parsed?.chainId ?? Number(parseTransaction(hex).chainId);
    return this.rpc(chainId).sendRawTransaction(hex);
  }

  fmtNative(n: bigint) { return fmtNativeEvm(n, 1); }
}
