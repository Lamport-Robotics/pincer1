/**
 * EVM transaction parser. Unsigned RLP (legacy type 0 with EIP-155 chain id, or EIP-1559 type 2) → an
 * owner-centric view: chain id, value, destination, recognised ERC-20 / ERC-721 / Permit2 calldata, the
 * worst-case fee, and the dangers the controller must see declared.
 *
 * Mirrors what the consent controller decodes on-device (PROTOCOL §1 "Multi-chain signing"): the daemon
 * explains, the controller enforces, and the two must agree.
 */
import { decodeFunctionData, getAddress, parseTransaction, type Address, type Hex, type TransactionSerializable } from "viem";
import type { ChainDanger, ChainParsed, ChainTokenMove, ParseContext } from "../types.js";
import { ERC20, ERC20_EXTRA, PERMIT2, PERMIT2_ABI, SEL, UNLIMITED_FLOOR, selectorOf } from "./abi.js";

export type EvmCall =
  | { fn: "transfer"; token: Address; to: Address; amount: bigint }
  | { fn: "transferFrom"; token: Address; from: Address; to: Address; amount: bigint }
  | { fn: "approve"; token: Address; spender: Address; amount: bigint; unlimited: boolean }
  | { fn: "increaseAllowance"; token: Address; spender: Address; amount: bigint; unlimited: boolean }
  | { fn: "decreaseAllowance"; token: Address; spender: Address; amount: bigint }
  | { fn: "setApprovalForAll"; collection: Address; operator: Address; approved: boolean }
  | { fn: "permit"; token: Address; owner: Address; spender: Address; amount: bigint; unlimited: boolean; deadline: bigint }
  | { fn: "permit2Approve"; token: Address; spender: Address; amount: bigint; unlimited: boolean; expiration: bigint }
  | { fn: "permit2TransferFrom"; token: Address; from: Address; to: Address; amount: bigint }
  | { fn: "router"; router: Address; selector: Hex }
  | { fn: "unknown"; selector: Hex | null }
  | { fn: "create" };

export interface EvmParsed extends ChainParsed {
  chain: "evm";
  chainId: number;
  type: "legacy" | "eip1559" | "eip2930";
  nonce: number;
  to: Address | null;
  value: bigint;
  data: Hex;
  gas: bigint;
  /** gasPrice for legacy, maxFeePerGas for typed transactions. */
  maxFeePerGas: bigint;
  /** gas × maxFeePerGas: the most this transaction can burn in fees. */
  maxFeeWei: bigint;
  call: EvmCall;
  /** The chain id the dapp said it was requesting, when known. */
  requestedChainId?: number;
  /** viem's parsed form, kept for re-serialisation with the signature. */
  tx: TransactionSerializable;
}

export interface EvmParseOptions {
  /** Lowercased router addresses whose calldata the controller does not decode. */
  routers: Set<string>;
}

const lower = (a: string) => a.toLowerCase();
const isUnlimited = (n: bigint) => n >= UNLIMITED_FLOOR;

/** True when the bytes deserialize as an EVM transaction (used to refuse disguised `signMessage` payloads). */
export function looksLikeEvmTx(bytes: Uint8Array): boolean {
  try {
    const hex = ("0x" + Buffer.from(bytes).toString("hex")) as Hex;
    const t = parseTransaction(hex);
    return t.chainId != null && t.gas != null;
  } catch {
    return false;
  }
}

function decodeCall(to: Address | null, data: Hex, owner: Address, routers: Set<string>): EvmCall {
  if (to == null) return { fn: "create" };
  if (!data || data === "0x") return { fn: "unknown", selector: null };
  const sel = selectorOf(data);
  const toL = lower(to);
  try {
    if (toL === lower(PERMIT2)) {
      const d = decodeFunctionData({ abi: PERMIT2_ABI, data });
      if (d.functionName === "approve") { const [token, spender, amount, expiration] = d.args; return { fn: "permit2Approve", token: getAddress(token), spender: getAddress(spender), amount, unlimited: isUnlimited(amount), expiration: BigInt(expiration) }; }
      if (d.functionName === "transferFrom") { const [from, toA, amount, token] = d.args; return { fn: "permit2TransferFrom", token: getAddress(token), from: getAddress(from), to: getAddress(toA), amount }; }
    }
    switch (sel) {
      case SEL.transfer: { const d = decodeFunctionData({ abi: ERC20, data }); if (d.functionName === "transfer") { const [toA, amount] = d.args; return { fn: "transfer", token: to, to: getAddress(toA), amount }; } break; }
      case SEL.approve: { const d = decodeFunctionData({ abi: ERC20, data }); if (d.functionName === "approve") { const [spender, amount] = d.args; return { fn: "approve", token: to, spender: getAddress(spender), amount, unlimited: isUnlimited(amount) }; } break; }
      case SEL.transferFrom: { const d = decodeFunctionData({ abi: ERC20, data }); if (d.functionName === "transferFrom") { const [from, toA, amount] = d.args; return { fn: "transferFrom", token: to, from: getAddress(from), to: getAddress(toA), amount }; } break; }
      case SEL.increaseAllowance: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "increaseAllowance") { const [spender, amount] = d.args; return { fn: "increaseAllowance", token: to, spender: getAddress(spender), amount, unlimited: isUnlimited(amount) }; } break; }
      case SEL.decreaseAllowance: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "decreaseAllowance") { const [spender, amount] = d.args; return { fn: "decreaseAllowance", token: to, spender: getAddress(spender), amount }; } break; }
      case SEL.setApprovalForAll: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "setApprovalForAll") { const [operator, approved] = d.args; return { fn: "setApprovalForAll", collection: to, operator: getAddress(operator), approved }; } break; }
      case SEL.safeTransferFrom721:
      case SEL.safeTransferFrom1155: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "safeTransferFrom") { const [from, toA] = d.args as readonly [Address, Address, ...unknown[]]; const amount = d.args.length >= 5 ? (d.args[3] as bigint) : 1n; return { fn: "transferFrom", token: to, from: getAddress(from), to: getAddress(toA), amount }; } break; }
      case SEL.permit2612: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "permit" && d.args.length === 7 && typeof d.args[4] !== "boolean") { const [o, spender, value, deadline] = d.args as readonly [Address, Address, bigint, bigint, ...unknown[]]; return { fn: "permit", token: to, owner: getAddress(o), spender: getAddress(spender), amount: value, unlimited: isUnlimited(value), deadline }; } break; }
      case SEL.permitDai: { const d = decodeFunctionData({ abi: ERC20_EXTRA, data }); if (d.functionName === "permit" && d.args.length === 8) { const [h, spender, , expiry, allowed] = d.args as readonly [Address, Address, bigint, bigint, boolean, ...unknown[]]; return { fn: "permit", token: to, owner: getAddress(h), spender: getAddress(spender), amount: allowed ? UNLIMITED_FLOOR << 1n : 0n, unlimited: allowed, deadline: expiry }; } break; }
    }
  } catch { /* malformed arguments for a known selector: fall through to unknown */ }
  if (routers.has(toL)) return { fn: "router", router: to, selector: sel ?? "0x00000000" };
  void owner;
  return { fn: "unknown", selector: sel };
}

export function parseEvmTx(bytes: Uint8Array, ctx: ParseContext, opts: EvmParseOptions): EvmParsed {
  const hex = ("0x" + Buffer.from(bytes).toString("hex")) as Hex;
  const t = parseTransaction(hex);
  if (t.r != null || t.s != null) throw new Error("transaction is already signed");
  if (t.type === "eip4844" || t.type === "eip7702") throw new Error(`unsupported transaction type ${t.type}`);
  if (t.chainId == null) throw new Error("transaction has no chain id (pre-EIP-155 replayable transactions are refused)");
  const chainId = Number(t.chainId);
  const to = t.to ? getAddress(t.to) : null;
  const value = t.value ?? 0n;
  const data = (t.data ?? "0x") as Hex;
  const gas = t.gas ?? 0n;
  const maxFeePerGas = t.type === "legacy" ? (t.gasPrice ?? 0n) : t.type === "eip2930" ? (t.gasPrice ?? 0n) : (t.maxFeePerGas ?? 0n);
  const owner = getAddress(ctx.owner);
  const call = decodeCall(to, data, owner, opts.routers);

  const tokenOut: ChainTokenMove[] = [];
  const dangers: ChainDanger[] = [];
  const counterparties = new Set<string>();
  if (to) counterparties.add(to);
  switch (call.fn) {
    case "transfer": tokenOut.push({ token: call.token, amount: call.amount, dest: call.to }); counterparties.add(call.to); break;
    case "transferFrom":
    case "permit2TransferFrom":
      if (lower(call.from) === lower(owner)) tokenOut.push({ token: call.token, amount: call.amount, dest: call.to });
      counterparties.add(call.to); break;
    case "approve":
    case "increaseAllowance":
    case "permit2Approve":
      if (call.amount > 0n) dangers.push({ kind: call.unlimited ? "approve_unlimited" : "approve", detail: call.unlimited ? "unlimited token allowance" : "token allowance", to: call.spender, amount: call.amount });
      counterparties.add(call.spender); break;
    case "permit":
      if (lower(call.owner) === lower(owner) && call.amount > 0n) dangers.push({ kind: call.unlimited ? "approve_unlimited" : "approve", detail: call.unlimited ? "unlimited token allowance by permit" : "token allowance by permit", to: call.spender, amount: call.amount });
      counterparties.add(call.spender); break;
    case "setApprovalForAll":
      if (call.approved) dangers.push({ kind: "set_approval_for_all", detail: "control over a whole collection", to: call.operator });
      counterparties.add(call.operator); break;
    case "create": dangers.push({ kind: "unknown_contract", detail: "deploys a new contract" }); break;
    case "unknown": if (call.selector) dangers.push({ kind: "unknown_contract", detail: "calls a function I don't recognise", to: to ?? undefined }); break;
    case "router": break;
  }

  const programs: string[] = [];
  if (to && data !== "0x") programs.push(to);
  const dest = value > 0n && to ? to : call.fn === "transfer" ? call.to : (call.fn === "transferFrom" || call.fn === "permit2TransferFrom") ? call.to : to;

  return {
    chain: "evm", chainId, type: t.type as EvmParsed["type"], nonce: Number(t.nonce ?? 0), to, value, data, gas, maxFeePerGas, maxFeeWei: gas * maxFeePerGas, call,
    requestedChainId: ctx.requestedChainId, tx: t,
    nativeOut: value, nativeUnit: "wei", dest, tokenOut, dangers, programs, signers: [owner],
    counterparties: [...counterparties].filter((a) => lower(a) !== lower(owner)),
  };
}
