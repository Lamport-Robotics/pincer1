/**
 * Independent verification of a router-built swap transaction. The quote said "at least X out"; the bytes
 * must say the same, and must go to the allowlisted router. Anything the decoder does not understand is
 * refused: a swap we cannot read is a swap we do not sign.
 */
import { createHash } from "node:crypto";
import { decodeFunctionData, getAddress, parseAbi, type Address, type Hex } from "viem";
import { ownerAtaSet, type ParsedTx } from "../tx/parse.js";
import { ATA, COMPUTE_BUDGET, JUPITER_V6, MEMO, MEMO_V1, SYSTEM, TOKEN, TOKEN_2022 } from "../tx/known.js";

export class SwapVerifyError extends Error { constructor(readonly code: string, message: string) { super(message); } }

// ── Jupiter v6 ──
const anchor = (name: string) => createHash("sha256").update(`global:${name}`).digest().subarray(0, 8).toString("hex");
const JUP_IX = {
  route: anchor("route"),
  sharedAccountsRoute: anchor("shared_accounts_route"),
  routeWithTokenLedger: anchor("route_with_token_ledger"),
  sharedAccountsRouteWithTokenLedger: anchor("shared_accounts_route_with_token_ledger"),
  exactOutRoute: anchor("exact_out_route"),
  sharedAccountsExactOutRoute: anchor("shared_accounts_exact_out_route"),
} as const;
export const JUPITER_DISCRIMINATORS = JUP_IX;

const u64le = (d: Uint8Array, o: number) => new DataView(d.buffer, d.byteOffset + o, 8).getBigUint64(0, true);
const u16le = (d: Uint8Array, o: number) => new DataView(d.buffer, d.byteOffset + o, 2).getUint16(0, true);
const PLUMBING = new Set([SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET, MEMO, MEMO_V1]);

export interface JupiterRoute { inAmount: bigint | null; quotedOut: bigint; slippageBps: number; minOut: bigint }

/** Decode the route instruction's trailing fields: [in_amount u64] quoted_out_amount u64, slippage_bps u16, platform_fee_bps u8. */
export function decodeJupiterRoute(data: Uint8Array): JupiterRoute {
  const disc = Buffer.from(data.subarray(0, 8)).toString("hex");
  const tail = (hasIn: boolean) => {
    const n = hasIn ? 19 : 11;
    if (data.length < 8 + n) throw new SwapVerifyError("bad_route", "Jupiter route instruction is too short");
    const quotedOut = u64le(data, data.length - 11);
    const slippageBps = u16le(data, data.length - 3);
    const inAmount = hasIn ? u64le(data, data.length - 19) : null;
    return { inAmount, quotedOut, slippageBps, minOut: (quotedOut * BigInt(10_000 - slippageBps)) / 10_000n };
  };
  switch (disc) {
    case JUP_IX.route: case JUP_IX.sharedAccountsRoute: return tail(true);
    case JUP_IX.routeWithTokenLedger: case JUP_IX.sharedAccountsRouteWithTokenLedger: return tail(false);
    case JUP_IX.exactOutRoute: case JUP_IX.sharedAccountsExactOutRoute: throw new SwapVerifyError("exact_out", "exact-out routes are not used by this wallet");
    default: throw new SwapVerifyError("unknown_route", "unrecognised Jupiter instruction");
  }
}

export interface JupiterLegs { owner: string; inMint: string; outMint: string }

/** Account positions in the Jupiter v6 route instructions (IDL order). */
const JUP_ACCOUNTS: Record<string, { authority: number; userDst: number; srcMint: number | null; dstMint: number }> = {
  [JUP_IX.route]: { authority: 1, userDst: 3, srcMint: null, dstMint: 5 },
  [JUP_IX.routeWithTokenLedger]: { authority: 1, userDst: 3, srcMint: null, dstMint: 5 },
  [JUP_IX.sharedAccountsRoute]: { authority: 2, userDst: 6, srcMint: 7, dstMint: 8 },
  [JUP_IX.sharedAccountsRouteWithTokenLedger]: { authority: 2, userDst: 6, srcMint: 7, dstMint: 8 },
};

/**
 * The Solana swap tx must call only Jupiter (plus plumbing), with a decodable route whose minimum output is at
 * least `minOut`. With `legs`, the route's mints and the owner's destination account are checked too: a route that
 * "guarantees" the minimum in some other token, or pays it into someone else's account, is refused.
 */
export function verifyJupiterTx(parsed: ParsedTx, minOut: bigint, maxSlippageBps: number, maxIn?: bigint, legs?: JupiterLegs): JupiterRoute {
  const apps = parsed.programs.filter((p) => !PLUMBING.has(p));
  for (const p of apps) if (p !== JUPITER_V6) throw new SwapVerifyError("router", `transaction calls ${p}, not the Jupiter router`);
  const routeIx = parsed.instructions.filter((ix) => ix.program === JUPITER_V6);
  if (routeIx.length === 0) throw new SwapVerifyError("router", "transaction does not call Jupiter");
  let found: JupiterRoute | null = null; let foundIx: (typeof routeIx)[number] | null = null;
  for (const ix of routeIx) {
    try { found = decodeJupiterRoute(ix.data); foundIx = ix; break; } catch (e) { if ((e as SwapVerifyError).code === "exact_out") throw e; }
  }
  if (!found || !foundIx) throw new SwapVerifyError("unknown_route", "no decodable Jupiter route instruction");
  if (found.slippageBps > maxSlippageBps) throw new SwapVerifyError("slippage", `route slippage ${found.slippageBps} bps exceeds the cap of ${maxSlippageBps}`);
  if (found.minOut < minOut) throw new SwapVerifyError("min_out", `route guarantees ${found.minOut} but the quote promised at least ${minOut}`);
  if (maxIn != null && found.inAmount != null && found.inAmount > maxIn) throw new SwapVerifyError("amount", `route spends ${found.inAmount}, more than the ${maxIn} quoted`);
  if (legs) {
    const pos = JUP_ACCOUNTS[Buffer.from(foundIx.data.subarray(0, 8)).toString("hex")];
    const acc = foundIx.accounts;
    if (!pos || acc.length <= pos.dstMint) throw new SwapVerifyError("unknown_route", "Jupiter route instruction has too few accounts");
    if (acc[pos.authority] !== legs.owner) throw new SwapVerifyError("router", "the route's transfer authority is not you");
    if (pos.srcMint != null && acc[pos.srcMint] !== legs.inMint) throw new SwapVerifyError("amount", `the route spends mint ${acc[pos.srcMint]}, not the ${legs.inMint} quoted`);
    if (acc[pos.dstMint] !== legs.outMint) throw new SwapVerifyError("amount", `the route pays out mint ${acc[pos.dstMint]}, not the ${legs.outMint} quoted`);
    if (!ownerAtaSet(legs.owner, legs.outMint).has(acc[pos.userDst])) throw new SwapVerifyError("receiver", "the route's output account is not your token account");
    for (const t of parsed.tokenOut) if (t.mint && t.mint !== legs.inMint) throw new SwapVerifyError("amount", `the transaction also moves ${t.mint}, which is not part of the quote`);
  }
  return found;
}

// ── 1inch v6 ──
export const ONEINCH_ABI = parseAbi([
  "function swap(address executor, (address srcToken, address dstToken, address srcReceiver, address dstReceiver, uint256 amount, uint256 minReturnAmount, uint256 flags) desc, bytes data) payable returns (uint256 returnAmount, uint256 spentAmount)",
  "function unoswap(uint256 token, uint256 amount, uint256 minReturn, uint256 dex) returns (uint256 returnAmount)",
  "function unoswap2(uint256 token, uint256 amount, uint256 minReturn, uint256 dex, uint256 dex2) returns (uint256 returnAmount)",
  "function unoswap3(uint256 token, uint256 amount, uint256 minReturn, uint256 dex, uint256 dex2, uint256 dex3) returns (uint256 returnAmount)",
  "function unoswapTo(uint256 to, uint256 token, uint256 amount, uint256 minReturn, uint256 dex) returns (uint256 returnAmount)",
  "function unoswapTo2(uint256 to, uint256 token, uint256 amount, uint256 minReturn, uint256 dex, uint256 dex2) returns (uint256 returnAmount)",
  "function unoswapTo3(uint256 to, uint256 token, uint256 amount, uint256 minReturn, uint256 dex, uint256 dex2, uint256 dex3) returns (uint256 returnAmount)",
  "function ethUnoswap(uint256 minReturn, uint256 dex) payable returns (uint256 returnAmount)",
  "function ethUnoswap2(uint256 minReturn, uint256 dex, uint256 dex2) payable returns (uint256 returnAmount)",
  "function ethUnoswap3(uint256 minReturn, uint256 dex, uint256 dex2, uint256 dex3) payable returns (uint256 returnAmount)",
  "function ethUnoswapTo(uint256 to, uint256 minReturn, uint256 dex) payable returns (uint256 returnAmount)",
  "function ethUnoswapTo2(uint256 to, uint256 minReturn, uint256 dex, uint256 dex2) payable returns (uint256 returnAmount)",
  "function ethUnoswapTo3(uint256 to, uint256 minReturn, uint256 dex, uint256 dex2, uint256 dex3) payable returns (uint256 returnAmount)",
]);
export const NATIVE_SENTINEL = "0xEeeeeEeeeEeEeeEeEeEeeEEEeeeeEeeeeeeeEEeE";
const ADDR_MASK = (1n << 160n) - 1n;
const addrOfUint = (u: bigint) => getAddress(("0x" + (u & ADDR_MASK).toString(16).padStart(40, "0")) as Hex);
const isNative = (a: string) => a.toLowerCase() === NATIVE_SENTINEL.toLowerCase() || /^0x0{40}$/.test(a);

export interface EvmSwapDecoded { minOut: bigint; amountIn: bigint | null; srcToken: Address | null; receiver: Address | null; /** null: native, or the calldata does not name it */ dstToken: Address | null }

export function decodeOneInch(data: Hex, value: bigint): EvmSwapDecoded {
  let d;
  try { d = decodeFunctionData({ abi: ONEINCH_ABI, data }); } catch { throw new SwapVerifyError("unknown_calldata", "1inch calldata is not a function I can decode"); }
  switch (d.functionName) {
    case "swap": { const [, desc] = d.args; return { minOut: desc.minReturnAmount, amountIn: desc.amount, srcToken: isNative(desc.srcToken) ? null : getAddress(desc.srcToken), receiver: /^0x0{40}$/.test(desc.dstReceiver) ? null : getAddress(desc.dstReceiver), dstToken: isNative(desc.dstToken) ? null : getAddress(desc.dstToken) }; }
    case "unoswap": case "unoswap2": case "unoswap3": { const [token, amount, minReturn] = d.args; return { minOut: minReturn, amountIn: amount, srcToken: addrOfUint(token), receiver: null, dstToken: null }; }
    case "unoswapTo": case "unoswapTo2": case "unoswapTo3": { const [to, token, amount, minReturn] = d.args; return { minOut: minReturn, amountIn: amount, srcToken: addrOfUint(token), receiver: addrOfUint(to), dstToken: null }; }
    case "ethUnoswap": case "ethUnoswap2": case "ethUnoswap3": { const [minReturn] = d.args; return { minOut: minReturn, amountIn: value, srcToken: null, receiver: null, dstToken: null }; }
    case "ethUnoswapTo": case "ethUnoswapTo2": case "ethUnoswapTo3": { const [to, minReturn] = d.args; return { minOut: minReturn, amountIn: value, srcToken: null, receiver: addrOfUint(to), dstToken: null }; }
  }
}

// ── 0x Exchange Proxy (v1 API) ──
export const ZEROX_ABI = parseAbi([
  "function transformERC20(address inputToken, address outputToken, uint256 inputTokenAmount, uint256 minOutputTokenAmount, (uint32 deploymentNonce, bytes data)[] transformations) payable returns (uint256 outputTokenAmount)",
]);
export function decodeZeroX(data: Hex, value: bigint): EvmSwapDecoded {
  let d;
  try { d = decodeFunctionData({ abi: ZEROX_ABI, data }); } catch { throw new SwapVerifyError("unknown_calldata", "0x calldata is not transformERC20; refusing a swap I cannot read"); }
  const [inputToken, outputToken, inputTokenAmount, minOutputTokenAmount] = d.args;
  return { minOut: minOutputTokenAmount, amountIn: isNative(inputToken) ? value : inputTokenAmount, srcToken: isNative(inputToken) ? null : getAddress(inputToken), receiver: null, dstToken: isNative(outputToken) ? null : getAddress(outputToken) };
}

// ── THORChain ──
export const THOR_ROUTER_ABI = parseAbi([
  "function depositWithExpiry(address vault, address asset, uint256 amount, string memo, uint256 expiration) payable",
]);
export interface ThorMemo { toAsset: string; dest: string; limit: bigint | null }
/** "=:ETH.ETH:0xabc…:12345" (also "SWAP:" / "s:"); the limit is in THORChain's 1e8 base units. Streaming suffixes ("/i/n") are read off the limit field. */
export function parseThorMemo(memo: string): ThorMemo {
  const parts = memo.split(":");
  const tag = (parts[0] ?? "").toLowerCase();
  if (!["=", "swap", "s"].includes(tag) || parts.length < 3) throw new SwapVerifyError("memo", "not a THORChain swap memo");
  const limitField = (parts[3] ?? "").split("/")[0];
  let limit: bigint | null = null;
  if (limitField) { try { limit = BigInt(limitField); } catch { throw new SwapVerifyError("memo", "unreadable limit in THORChain memo"); } }
  return { toAsset: parts[1], dest: parts[2], limit };
}
export function decodeThorDeposit(data: Hex): { vault: Address; asset: Address; amount: bigint; memo: string; expiration: bigint } {
  let d;
  try { d = decodeFunctionData({ abi: THOR_ROUTER_ABI, data }); } catch { throw new SwapVerifyError("unknown_calldata", "THORChain router calldata is not depositWithExpiry"); }
  const [vault, asset, amount, memo, expiration] = d.args;
  return { vault: getAddress(vault), asset: getAddress(asset), amount, memo, expiration };
}

/** THORChain base units (1e8) → the asset's own decimals. */
export const fromThorUnits = (n: bigint, decimals: number) => (decimals >= 8 ? n * 10n ** BigInt(decimals - 8) : n / 10n ** BigInt(8 - decimals));
export const toThorUnits = (n: bigint, decimals: number) => (decimals >= 8 ? n / 10n ** BigInt(decimals - 8) : n * 10n ** BigInt(8 - decimals));
