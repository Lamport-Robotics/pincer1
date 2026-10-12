/**
 * Swap route providers. Each one knows how to quote and how to obtain router-built transaction bytes;
 * none of them is trusted: everything they return is re-decoded in verify.ts before the controller is asked.
 */
import { encodeFunctionData, getAddress, serializeTransaction, type Address, type Hex } from "viem";
import { ERC20 } from "../chains/evm/abi.js";
import type { EvmRpc } from "../chains/evm/rpc.js";
import { ONEINCH_V6_ROUTER, ZEROX_EXCHANGE_PROXY } from "./routers.js";
import { NATIVE_SENTINEL } from "./verify.js";

export type FetchLike = typeof fetch;
const json = async (r: Response, what: string) => { if (!r.ok) throw new Error(`${what} ${r.status}: ${(await r.text()).slice(0, 200)}`); return r.json() as Promise<any>; };
const timeout = () => AbortSignal.timeout(8000);

// ── Jupiter (Solana) ──
export class JupiterProvider {
  constructor(private base = "https://lite-api.jup.ag/swap/v1", private fetchImpl: FetchLike = fetch) {}
  async quote(inputMint: string, outputMint: string, amount: bigint, slippageBps: number): Promise<{ outAmount: bigint; raw: any }> {
    const u = `${this.base}/quote?inputMint=${inputMint}&outputMint=${outputMint}&amount=${amount}&slippageBps=${slippageBps}&restrictIntermediateTokens=true&swapMode=ExactIn`;
    const j = await json(await this.fetchImpl(u, { signal: timeout() }), "jupiter quote");
    return { outAmount: BigInt(j.outAmount), raw: j };
  }
  async build(quoteRaw: any, userPublicKey: string): Promise<Uint8Array> {
    const r = await this.fetchImpl(`${this.base}/swap`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ quoteResponse: quoteRaw, userPublicKey, wrapAndUnwrapSol: true, dynamicComputeUnitLimit: true, prioritizationFeeLamports: { priorityLevelWithMaxLamports: { maxLamports: 1_000_000, priorityLevel: "medium" } } }), signal: timeout() });
    const j = await json(r, "jupiter swap");
    return new Uint8Array(Buffer.from(String(j.swapTransaction), "base64"));
  }
}

// ── EVM aggregators ──
export interface EvmRouteTx { to: Address; data: Hex; value: bigint; gas: bigint | null; gasPrice: bigint | null }
export interface EvmAggregator {
  readonly name: "1inch" | "0x";
  readonly router: Address;
  supports(chainId: number): boolean;
  quote(chainId: number, src: string, dst: string, amount: bigint): Promise<{ outAmount: bigint }>;
  build(chainId: number, src: string, dst: string, amount: bigint, from: Address, slippageBps: number): Promise<{ outAmount: bigint; tx: EvmRouteTx }>;
}

export class OneInchProvider implements EvmAggregator {
  readonly name = "1inch" as const;
  readonly router = ONEINCH_V6_ROUTER as Address;
  constructor(private apiKey: string, private base = "https://api.1inch.dev/swap/v6.0", private fetchImpl: FetchLike = fetch) {}
  private headers() { return { Authorization: `Bearer ${this.apiKey}`, accept: "application/json" }; }
  supports(chainId: number) { return [1, 8453, 42161, 56, 137].includes(chainId) && !!this.apiKey; }
  async quote(chainId: number, src: string, dst: string, amount: bigint) {
    const j = await json(await this.fetchImpl(`${this.base}/${chainId}/quote?src=${src}&dst=${dst}&amount=${amount}`, { headers: this.headers(), signal: timeout() }), "1inch quote");
    return { outAmount: BigInt(j.dstAmount) };
  }
  async build(chainId: number, src: string, dst: string, amount: bigint, from: Address, slippageBps: number) {
    const slippage = (slippageBps / 100).toString();
    const u = `${this.base}/${chainId}/swap?src=${src}&dst=${dst}&amount=${amount}&from=${from}&origin=${from}&slippage=${slippage}&disableEstimate=true&allowPartialFill=false&usePermit2=false`;
    const j = await json(await this.fetchImpl(u, { headers: this.headers(), signal: timeout() }), "1inch swap");
    return { outAmount: BigInt(j.dstAmount), tx: { to: getAddress(j.tx.to), data: j.tx.data as Hex, value: BigInt(j.tx.value ?? 0), gas: j.tx.gas != null ? BigInt(j.tx.gas) : null, gasPrice: j.tx.gasPrice != null ? BigInt(j.tx.gasPrice) : null } };
  }
}

const ZEROX_HOSTS: Record<number, string> = { 1: "https://api.0x.org", 8453: "https://base.api.0x.org", 42161: "https://arbitrum.api.0x.org", 56: "https://bsc.api.0x.org", 137: "https://polygon.api.0x.org" };
export class ZeroXProvider implements EvmAggregator {
  readonly name = "0x" as const;
  readonly router = ZEROX_EXCHANGE_PROXY as Address;
  constructor(private apiKey: string, private hosts: Record<number, string> = ZEROX_HOSTS, private fetchImpl: FetchLike = fetch) {}
  private headers() { return { "0x-api-key": this.apiKey, accept: "application/json" }; }
  supports(chainId: number) { return !!this.hosts[chainId] && !!this.apiKey; }
  private sym = (a: string) => (a.toLowerCase() === NATIVE_SENTINEL.toLowerCase() ? "ETH" : a);
  async quote(chainId: number, src: string, dst: string, amount: bigint) {
    const j = await json(await this.fetchImpl(`${this.hosts[chainId]}/swap/v1/price?sellToken=${this.sym(src)}&buyToken=${this.sym(dst)}&sellAmount=${amount}`, { headers: this.headers(), signal: timeout() }), "0x price");
    return { outAmount: BigInt(j.buyAmount) };
  }
  async build(chainId: number, src: string, dst: string, amount: bigint, from: Address, slippageBps: number) {
    const u = `${this.hosts[chainId]}/swap/v1/quote?sellToken=${this.sym(src)}&buyToken=${this.sym(dst)}&sellAmount=${amount}&takerAddress=${from}&slippagePercentage=${(slippageBps / 10_000).toString()}`;
    const j = await json(await this.fetchImpl(u, { headers: this.headers(), signal: timeout() }), "0x quote");
    return { outAmount: BigInt(j.buyAmount), tx: { to: getAddress(j.to), data: j.data as Hex, value: BigInt(j.value ?? 0), gas: j.gas != null ? BigInt(j.gas) : null, gasPrice: j.gasPrice != null ? BigInt(j.gasPrice) : null } };
  }
}

// ── THORChain (cross-chain) ──
export interface ThorQuote { inboundAddress: string; router: string | null; expectedOut: bigint; memo: string; expiry: number; dustThreshold: bigint; recommendedMinIn: bigint; raw: any }
export class ThorchainProvider {
  constructor(private base = "https://thornode.ninerealms.com", private fetchImpl: FetchLike = fetch) {}
  /** amount in THORChain base units (1e8). `destination` is the owner's address on the destination chain. */
  async quote(fromAsset: string, toAsset: string, amount: bigint, destination: string, toleranceBps: number): Promise<ThorQuote> {
    const u = `${this.base.replace(/\/$/, "")}/thorchain/quote/swap?from_asset=${encodeURIComponent(fromAsset)}&to_asset=${encodeURIComponent(toAsset)}&amount=${amount}&destination=${encodeURIComponent(destination)}&tolerance_bps=${toleranceBps}`;
    const j = await json(await this.fetchImpl(u, { signal: timeout() }), "thorchain quote");
    if (!j.inbound_address || !j.expected_amount_out || !j.memo) throw new Error("thorchain quote is incomplete");
    return { inboundAddress: String(j.inbound_address), router: j.router ? String(j.router) : null, expectedOut: BigInt(j.expected_amount_out), memo: String(j.memo), expiry: Number(j.expiry ?? 0), dustThreshold: BigInt(j.dust_threshold ?? 0), recommendedMinIn: BigInt(j.recommended_min_amount_in ?? 0), raw: j };
  }
}

// ── EVM transaction assembly for the swap module ──
export interface EvmTxBuild { chainId: number; from: Address; to: Address; data: Hex; value: bigint; gas: bigint | null; gasPrice: bigint | null; nonce: number }

/** Unsigned EIP-1559 RLP. Fees: the aggregator's gas price is treated as an upper bound; the RPC's priority fee is used if lower. */
export async function buildEvmTx(rpc: EvmRpc, b: EvmTxBuild): Promise<Uint8Array> {
  const fees = await rpc.fees().catch(() => ({ maxFeePerGas: b.gasPrice ?? 0n, maxPriorityFeePerGas: 0n }));
  const maxFeePerGas = b.gasPrice && b.gasPrice > 0n ? (fees.maxFeePerGas > 0n && fees.maxFeePerGas < b.gasPrice ? fees.maxFeePerGas : b.gasPrice) : fees.maxFeePerGas;
  if (maxFeePerGas <= 0n) throw new Error("could not determine a gas price");
  const maxPriorityFeePerGas = fees.maxPriorityFeePerGas > 0n && fees.maxPriorityFeePerGas < maxFeePerGas ? fees.maxPriorityFeePerGas : maxFeePerGas / 10n;
  let gas = b.gas;
  if (gas == null) gas = ((await rpc.estimateGas({ from: b.from, to: b.to, data: b.data, value: b.value })) * 12n) / 10n;
  const hex = serializeTransaction({ type: "eip1559", chainId: b.chainId, nonce: b.nonce, to: b.to, value: b.value, data: b.data, gas, maxFeePerGas, maxPriorityFeePerGas });
  return new Uint8Array(Buffer.from(hex.slice(2), "hex"));
}

export const approveCalldata = (spender: Address, amount: bigint): Hex => encodeFunctionData({ abi: ERC20, functionName: "approve", args: [spender, amount] });
