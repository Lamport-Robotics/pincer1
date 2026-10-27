/**
 * Chain adapter contract. One adapter per key family: Solana (ed25519), EVM and Bitcoin (secp256k1).
 *
 * The pipeline in sign.ts is chain-agnostic: it asks the adapter to parse, gather intel, explain, assess,
 * produce the claims the controller re-checks, and finally assemble and broadcast. The adapter never sees a
 * key; the only thing that can turn its bytes into a signature is the consent controller.
 *
 * Every adapter speaks in the same voice as tx/explain.ts: short, concrete, no hex, one danger per sentence.
 */
import type { RiskVerdict } from "../risk/filter.js";
import type { Explanation } from "../tx/explain.js";
import type { AnyClaims, SignKind } from "../controller/types.js";

export type ChainId = "solana" | "evm" | "bitcoin";

/** What leaves the owner's wallet, in the chain's smallest unit (lamports, wei, sats). */
export interface ChainTokenMove {
  /** Mint / contract address; null when the chain cannot tell from the bytes alone. */
  token: string | null;
  amount: bigint;
  dest: string;
}

export interface ChainDanger {
  kind: string;
  detail: string;
  /** The party that receives the power (spender, operator, new authority, …). */
  to?: string;
  amount?: bigint;
}

/** The common denominator every parsed transaction exposes; adapters extend it with chain detail. */
export interface ChainParsed {
  chain: ChainId;
  /** Native value leaving the owner, in the chain's smallest unit. */
  nativeOut: bigint;
  nativeUnit: "lamports" | "wei" | "sats";
  /** Primary recipient (largest native recipient, or the contract called). */
  dest: string | null;
  tokenOut: ChainTokenMove[];
  dangers: ChainDanger[];
  /** Programs / contracts / routers the transaction touches. */
  programs: string[];
  /** Who must sign. The owner must be among them or the request is `wrong_owner`. */
  signers: string[];
  /** Addresses worth looking up before explaining (destinations, spenders, contracts). */
  counterparties: string[];
}

/** What the chain knows about an address. The Solana adapter maps its AddressIntel onto this. */
export interface ChainAddressIntel {
  address: string;
  exists: boolean;
  /** Executable program / contract code present. */
  isContract: boolean;
  /** Days since creation / first activity; null when unknown. */
  ageDays: number | null;
  txCount: number;
  txCountIsLowerBound: boolean;
  /** Native balance in the smallest unit; null when unknown. */
  balance: bigint | null;
  /** Source code verified on a block explorer (EVM only; null = unknown / no key). */
  verified?: boolean | null;
  fetchedAt: number;
}

export interface ChainTokenMeta { symbol?: string; decimals?: number }

export interface ChainIntel {
  address(a: string): Promise<ChainAddressIntel>;
  token(a: string): Promise<ChainTokenMeta | null>;
  ownerBalance(owner: string): Promise<bigint | null>;
}

export interface ParseContext {
  owner: string;
  origin?: string;
  /** The chain id the dapp asked for (EVM); a transaction for another chain is blocked. */
  requestedChainId?: number;
}

export interface ExplainCtx {
  origin?: string;
  intel: Record<string, ChainAddressIntel>;
  tokenMeta: Record<string, ChainTokenMeta>;
  addressBook: Record<string, string>;
  ownerBalance: bigint | null;
}

export interface AssessCtx extends ExplainCtx {
  owner: string;
  originAllowlist: string[];
  dailyLimit: bigint;
  /** Programs / contracts the owner approved by hand; everything else is unknown on every request. */
  knownPrograms: Set<string>;
  /** Routers the owner (or the swap module) allowlisted for this chain. */
  routers: Set<string>;
  /** Owner setting: unlimited approvals are tolerated when the spender is an allowlisted router. */
  unlimitedApproveToRouters: boolean;
}

export interface ChainAdapter<P extends ChainParsed = ChainParsed> {
  readonly id: ChainId;
  /** Intel bound to the network this transaction is for (EVM has one per chain id). */
  intelFor(parsed: P): ChainIntel;
  /** Controller `sign` kind for transactions of this chain. */
  readonly signKind: SignKind;
  parse(bytes: Uint8Array, ctx: ParseContext): Promise<P>;
  /** The exact bytes the controller must see (Solana: the message; EVM: the unsigned RLP; Bitcoin: the PSBT). */
  signingBytes(bytes: Uint8Array, parsed: P): Uint8Array;
  explain(parsed: P, ctx: ExplainCtx): Explanation;
  assess(parsed: P, ctx: AssessCtx): RiskVerdict;
  claims(parsed: P): AnyClaims;
  /** Combine the dapp's unsigned bytes with the controller's output into something a node accepts. */
  assemble(unsignedBytes: Uint8Array, controllerOutput: Uint8Array, parsed: P, owner: string): Promise<Uint8Array>;
  /** Send to the network; returns the chain's transaction id / signature. */
  broadcast(bytes: Uint8Array, parsed?: P): Promise<string>;
  /** Token contracts/mints whose symbol and decimals make the explanation readable (not only the ones leaving). */
  tokens?(parsed: P): string[];
  /** Allowlisted routers for this chain, lowercased where the chain is case-insensitive. */
  readonly routers?: Set<string>;
  /** Format a native amount for speech ("0.5 SOL", "0.0015 BTC"). */
  fmtNative(amount: bigint): string;
  /** Spoken name of the native unit's big denomination, for the two-person rule and the daily-limit line. */
  readonly nativeSymbol: string;
  readonly nativeDecimals: number;
}
