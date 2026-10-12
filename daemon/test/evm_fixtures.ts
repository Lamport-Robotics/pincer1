/** Hand-built EVM transactions (viem serializeTransaction) and a fake JSON-RPC for the multi-chain tests. */
import { encodeFunctionData, erc20Abi, maxUint256, serializeTransaction, type Address, type Hex } from "viem";
import type { EvmRpc } from "../src/chains/evm/rpc.js";
import { Connection } from "@solana/web3.js";
import { SimController } from "../src/controller/sim.js";
import { MockRobot } from "../src/presence/client.js";
import { Signer } from "../src/sign.js";
import { Intel } from "../src/risk/intel.js";
import { EvmAdapter } from "../src/chains/evm/index.js";
import type { Config } from "../src/config.js";
import { ONEINCH_V6_ROUTER } from "../src/swap/routers.js";

export const USDC = "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48" as Address; // mainnet USDC
export const WETH = "0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2" as Address;
export const STRANGER = ("0x1234" + "0".repeat(32) + "1234") as Address; // 0x1234…1234
export const FRIEND = "0x00000000000000000000000000000000000000f1" as Address;
export const EOA_SPENDER = "0x000000000000000000000000000000000000dead" as Address;
export const CONTRACT = "0x00000000000000000000000000000000000c0de1" as Address;
export const ROUTER = ONEINCH_V6_ROUTER as Address;
export const GWEI = 10n ** 9n;
export const ETH = 10n ** 18n;

const hexBytes = (h: Hex) => new Uint8Array(Buffer.from(h.slice(2), "hex"));

export function tx1559(o: { chainId?: number; to: Address | null; value?: bigint; data?: Hex; gas?: bigint; maxFeePerGas?: bigint; nonce?: number }): Uint8Array {
  return hexBytes(serializeTransaction({ type: "eip1559", chainId: o.chainId ?? 1, nonce: o.nonce ?? 0, to: o.to ?? undefined, value: o.value ?? 0n, data: o.data ?? "0x", gas: o.gas ?? 21_000n, maxFeePerGas: o.maxFeePerGas ?? 20n * GWEI, maxPriorityFeePerGas: GWEI }));
}
export function txLegacy(o: { chainId?: number; to: Address; value?: bigint; data?: Hex; gas?: bigint; gasPrice?: bigint }): Uint8Array {
  return hexBytes(serializeTransaction({ type: "legacy", chainId: o.chainId ?? 1, nonce: 0, to: o.to, value: o.value ?? 0n, data: o.data ?? "0x", gas: o.gas ?? 21_000n, gasPrice: o.gasPrice ?? 20n * GWEI }));
}
export const erc20Transfer = (to: Address, amount: bigint) => encodeFunctionData({ abi: erc20Abi, functionName: "transfer", args: [to, amount] });
export const erc20Approve = (spender: Address, amount: bigint) => encodeFunctionData({ abi: erc20Abi, functionName: "approve", args: [spender, amount] });
export const approveUnlimited = (spender: Address) => erc20Approve(spender, maxUint256);

export interface FakeRpcState { code?: Record<string, Hex>; balance?: Record<string, bigint>; nonce?: Record<string, number>; calls?: Record<string, Hex>; sent?: Hex[]; blockNumber?: bigint; feeMaxFeePerGas?: bigint }

/** A chain in a box: code/balance/nonce tables, eth_call by selector, broadcast log. Every address keyed lowercase. */
export function fakeRpc(chainId: number, st: FakeRpcState = {}): EvmRpc & { st: Required<FakeRpcState> } {
  const s: Required<FakeRpcState> = { code: {}, balance: {}, nonce: {}, calls: {}, sent: [], blockNumber: 20_000_000n, feeMaxFeePerGas: 20n * GWEI, ...st };
  const L = (a: string) => a.toLowerCase();
  return {
    chainId, st: s,
    getCode: async (a) => s.code[L(a)] ?? "0x",
    getBalance: async (a) => s.balance[L(a)] ?? 0n,
    call: async (to, data) => s.calls[`${L(to)}:${data.slice(0, 10)}`] ?? s.calls[data.slice(0, 10)] ?? "0x",
    blockNumber: async () => s.blockNumber,
    blockTimestamp: async (n) => BigInt(1_700_000_000 + Number(n) * 12),
    transactionCount: async (a) => s.nonce[L(a)] ?? 0,
    fees: async () => ({ maxFeePerGas: s.feeMaxFeePerGas, maxPriorityFeePerGas: GWEI }),
    estimateGas: async () => 100_000n,
    sendRawTransaction: async (raw) => { s.sent.push(raw); return ("0x" + "ab".repeat(32)) as Hex; },
  };
}

export const cfg = { PINCER_MODE: "dev", addressBook: {}, originAllowlist: ["https://app.uniswap.org"], dailyLimitLamports: 2_000_000_000n, dailyLimitWei: ETH, dailyLimitSats: 5_000_000n } as unknown as Config;
export const seed = new Uint8Array(32).fill(7);

/** Sim + mock robot + Signer with the EVM adapter on a fake chain 1 (and 8453). */
export function evmHarness(opts: { autoTouch?: boolean; rpcState?: FakeRpcState; addressBook?: Record<string, string>; settings?: Record<string, unknown>; dailyLimitWei?: bigint } = {}) {
  const sim = new SimController({ seed, autoTouch: opts.autoTouch ?? true, dailyLimitWei: opts.dailyLimitWei });
  const robot = new MockRobot();
  const conn = new Connection("http://127.0.0.1:1");
  const rpc1 = fakeRpc(1, opts.rpcState);
  const rpc8453 = fakeRpc(8453, opts.rpcState);
  const evm = new EvmAdapter({ rpcs: new Map([[1, rpc1], [8453, rpc8453]]), intel: { timeoutMs: 200 } });
  const c = { ...cfg, addressBook: opts.addressBook ?? {} } as Config;
  const signer = new Signer(c, sim, robot, conn, () => sim.publicKey, new Intel(conn, 50), 500, () => ({ roomLock: false, roomMatchMin: 0.85, twoPersonSol: 0, speakBeforeSign: true, ...(opts.settings ?? {}) } as any));
  signer.addresses = () => sim.addresses;
  signer.register(evm);
  return { sim, robot, conn, evm, rpc1, rpc8453, signer, owner: sim.evmAddress as Address };
}
