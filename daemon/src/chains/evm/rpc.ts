/**
 * The handful of JSON-RPC calls the EVM adapter needs, behind an interface so tests run on a fake and the
 * daemon runs on viem. One client per chain id; URLs come from `EVM_RPC_<chainId>`.
 */
import { createPublicClient, http, type Hex, type Address } from "viem";

export interface EvmRpc {
  chainId: number;
  getCode(addr: Address, block?: bigint): Promise<Hex>;
  getBalance(addr: Address): Promise<bigint>;
  call(to: Address, data: Hex): Promise<Hex>;
  blockNumber(): Promise<bigint>;
  blockTimestamp(block: bigint): Promise<bigint>;
  transactionCount(addr: Address): Promise<number>;
  fees(): Promise<{ maxFeePerGas: bigint; maxPriorityFeePerGas: bigint }>;
  estimateGas(tx: { from: Address; to: Address; data?: Hex; value?: bigint }): Promise<bigint>;
  sendRawTransaction(raw: Hex): Promise<Hex>;
}

export const CHAIN_NAMES: Record<number, string> = { 1: "Ethereum", 8453: "Base", 42161: "Arbitrum", 56: "BNB Chain", 137: "Polygon" };
export const NATIVE_SYMBOL: Record<number, string> = { 1: "ETH", 8453: "ETH", 42161: "ETH", 56: "BNB", 137: "POL" };
export const DEFAULT_RPC: Record<number, string> = {
  1: "https://cloudflare-eth.com",
  8453: "https://mainnet.base.org",
  42161: "https://arb1.arbitrum.io/rpc",
  56: "https://bsc-dataseed.binance.org",
  137: "https://polygon-rpc.com",
};
export const SUPPORTED_CHAIN_IDS = Object.keys(DEFAULT_RPC).map(Number);

export function viemRpc(chainId: number, url: string, timeoutMs = 4000): EvmRpc {
  const client = createPublicClient({ transport: http(url, { timeout: timeoutMs, retryCount: 1 }) });
  return {
    chainId,
    getCode: async (addr, block) => (await client.getCode(block != null ? { address: addr, blockNumber: block } : { address: addr })) ?? "0x",
    getBalance: (addr) => client.getBalance({ address: addr }),
    call: async (to, data) => (await client.call({ to, data })).data ?? "0x",
    blockNumber: () => client.getBlockNumber(),
    blockTimestamp: async (n) => (await client.getBlock({ blockNumber: n })).timestamp,
    transactionCount: (addr) => client.getTransactionCount({ address: addr }),
    fees: async () => { const f = await client.estimateFeesPerGas(); return { maxFeePerGas: f.maxFeePerGas ?? 0n, maxPriorityFeePerGas: f.maxPriorityFeePerGas ?? 0n }; },
    estimateGas: (tx) => client.estimateGas({ account: tx.from, to: tx.to, data: tx.data, value: tx.value }),
    sendRawTransaction: (raw) => client.sendRawTransaction({ serializedTransaction: raw }),
  };
}

/** Build the chainId → rpc map from the environment (EVM_RPC_1, EVM_RPC_8453, …), defaults for the five known chains. */
export function rpcsFromEnv(env: Record<string, string | undefined> = process.env): Map<number, EvmRpc> {
  const out = new Map<number, EvmRpc>();
  for (const [k, v] of Object.entries(env)) {
    const m = /^EVM_RPC_(\d+)$/.exec(k);
    if (m && v) out.set(Number(m[1]), viemRpc(Number(m[1]), v));
  }
  for (const id of SUPPORTED_CHAIN_IDS) if (!out.has(id)) out.set(id, viemRpc(id, DEFAULT_RPC[id]));
  return out;
}
