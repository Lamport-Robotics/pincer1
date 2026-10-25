/** Messages on the daemon ⇄ consent controller link (docs/PROTOCOL.md §1). */

/** Solana claims (kind `tx`). Kept under its historical name; the chain-specific unions are below. */
export interface Claims {
  lamports_out: string;
  /** Every token account the owner pays from, with the receiving token account; the controller checks mint, amount and recipient. */
  token_out: { mint: string; amount: string; dest: string }[];
  dest: string;
  programs: string[];
  dangerous: string[];
  /** Base fee plus priority fee the owner pays; the controller refuses a message whose compute budget costs more. */
  fee_lamports?: string;
}
export type SolanaClaims = Claims;

/** EVM claims (kind `evm_tx`), PROTOCOL §1 "Multi-chain signing". */
export interface EvmClaims {
  chain_id: number;
  to: string;
  value_wei: string;
  /** Every ERC-20 amount leaving the owner, with its recipient; the controller checks token, amount and recipient. */
  token_out: { token: string; amount: string; to: string }[];
  /** gas_limit × max_fee_per_gas, the most the transaction can burn in fees. */
  max_fee_wei: string;
  /** Informational only. The controller keeps its own allowlist (built-in + owner-added with the PIN) and ignores this list. */
  routers: string[];
  dangerous: EvmDanger[];
}
export type PendingHook = (p: { need: string; parsed?: Record<string, unknown> }) => void;
export type EvmDanger = "approve_unlimited" | "approve" | "set_approval_for_all" | "unknown_contract" | "delegatecall";

/** Bitcoin claims (kind `btc_psbt`). */
export interface BtcClaims {
  network: "mainnet" | "testnet";
  sats_out: string;
  dest: string;
  fee_sats: string;
  inputs: number;
  dangerous: BtcDanger[];
}
export type BtcDanger = "high_fee" | "unknown_change" | "sighash_not_all";

/** Message claims. `evm_msg`: EIP-191 bytes, or an EIP-712 digest plus the typed-data JSON the controller re-hashes. */
export type MsgClaims =
  | { kind: "raw" }
  | { kind: "eip191" }
  | { kind: "eip712"; typed_data: string; digest: string };

export type AnyClaims = SolanaClaims | EvmClaims | BtcClaims | MsgClaims;

export interface Policy {
  daily_limit_lamports: string;
  spent_today_lamports: string;
  day: number;
  /** Per-chain limits (the controller never trusts a daemon-supplied fiat conversion). */
  daily_limit_wei?: string;
  spent_today_wei?: string;
  daily_limit_sats?: string;
  spent_today_sats?: string;
}

/** One seed, two key families. Every address the controller can sign for. */
export interface Addresses {
  solana: string;
  evm?: string;
  bitcoin?: string;
}

export interface Allowance {
  lamports: string;
  spent: string;
  programs: string[];
  until_unix: number;
  max_per_tx_lamports: string;
}

export interface InheritanceStatus {
  armed: boolean;
  inactivity_days: number;
  days_since_touch: number;
  powered_days?: number;
  heir?: string | null;
  warned?: number[];
}

export interface Hello {
  t: "hello";
  fw: string;
  se050: boolean;
  initialized: boolean;
  pubkey: string | null;
  /** Absent on firmware that predates multi-chain; the daemon then exposes Solana only. */
  addresses?: Addresses;
  locked: boolean;
  policy: Policy;
  allowance: Allowance | null;
  /** Whether a duress rhythm has been set. Whether duress is ACTIVE is never reported: the daemon cannot tell the decoy from the real wallet. */
  duress_pin_set?: boolean;
  /** Wrong PIN rhythms so far (the SE050 keeps the authoritative counter; 5 wipes the device). */
  pin_failures?: number;
  /** false until the init flow finished with set_pin; signing is refused until then. */
  pin_set?: boolean;
  /** true when the SE050 channel keys were compiled in (bench builds), never in production. */
  dev_keys?: boolean;
  /** the trusted display is present and initialised; prod refuses to run without it */
  display?: boolean;
  board?: string;
  inheritance?: { armed: boolean; inactivity_days: number; days_since_touch: number } | null;
  /** Firmware update state (PROTOCOL §1 "update"): which A/B partition runs, whether a verified image waits in the other one, and whether this image is still on trial (boots counted, not yet committed). Absent on firmware before 0.5.0. */
  update?: { partition: number; staged: boolean; trial: boolean };
}

export type UpdateReason = "not_supported" | "state" | "size" | "offset" | "length" | "flash" | "hash" | "image" | "version" | DenyReason;
export type UpdateBegin = { ok: true; chunk: number; partition_size: number } | { ok: false; reason: UpdateReason };
export type UpdateResult = { ok: true } | { ok: false; reason: UpdateReason };

/**
 * tx / msg: Solana message / arbitrary bytes (ed25519).
 * evm_tx: unsigned RLP (legacy or EIP-1559) → 65-byte r‖s‖v. evm_msg: EIP-191 bytes or an EIP-712 digest.
 * btc_psbt: BIP-174 PSBT → the same PSBT with the controller's partial signatures (returned in `sig`).
 * btc_msg: BIP-322 simple (reserved; not built by this daemon yet).
 */
export type SignKind = "tx" | "msg" | "evm_tx" | "evm_msg" | "btc_psbt" | "btc_msg";
export type PolicyChain = "solana" | "evm" | "bitcoin";

export type DenyReason = "timeout" | "claims_mismatch" | "over_daily_limit" | "locked" | "policy" | "user" | "duress" | "not_initialized" | "busy" | "pin_not_set" | "wrong_pin" | "pin_mismatch" | "wiped";

export type ControllerEvent = { t: "event"; ev: "touch" | "long_touch" | "tamper" | "rhythm" | "pin_failed" | "wiped" | "inheritance_warning"; what?: string; days_left?: number };

export interface ControllerClient {
  hello(): Promise<Hello>;
  /** First boot: the controller shows the 24 words on its own display; nothing secret crosses this link. */
  init(): Promise<{ pubkey: string; addresses?: Addresses; decoy_addresses?: Addresses }>;
  /** Second tap rhythm that opens the decoy wallet through `unlock`. PIN-gated; tapped twice; refused in duress. */
  setDuressPin(): Promise<{ ok: true } | { ok: false; reason: DenyReason }>;
  /** The decoy wallet's public addresses, so the owner can fund it. PIN-gated; refused in duress. */
  decoyAddresses(): Promise<{ ok: true; addresses: Addresses } | { ok: false; reason: DenyReason }>;
  /** First-boot only: the owner taps the PIN rhythm twice on the robot. */
  setPin(): Promise<boolean>;
  /** `onPending` fires once the controller has parsed the request itself and is waiting for a touch; `parsed` is the controller's own reading (what its display shows). */
  sign(kind: SignKind, bytes: Uint8Array, claims: AnyClaims, ttlMs?: number, onPending?: PendingHook): Promise<{ ok: true; sig: Uint8Array } | { ok: false; reason: DenyReason }>;
  elevate(limitLamports: bigint): Promise<boolean>;
  allowance(a: Omit<Allowance, "spent"> | { revoke: true }): Promise<boolean>;
  lock(): Promise<void>;
  unlock(): Promise<boolean>;
  duress(): Promise<void>;
  setPolicy(dailyLimitLamports: bigint): Promise<boolean>;
  /** Per-chain daily limit (wei for evm, sats for bitcoin). Long touch, like setPolicy. */
  setChainPolicy(chain: Exclude<PolicyChain, "solana">, limit: bigint): Promise<boolean>;
  time(unix: number): Promise<void>;
  /** Dead-man's switch: sign and store a durable-nonce tx on the controller; released only after inactivity. */
  inheritanceSet(bytes: Uint8Array, claims: Claims, cfg: { inactivityDays: number; warnDays: number[]; heir: string }): Promise<{ ok: true; sig: Uint8Array } | { ok: false; reason: DenyReason | "not_durable_nonce" }>;
  inheritanceStatus(): Promise<InheritanceStatus>;
  inheritanceRelease(): Promise<{ ok: true; tx: Uint8Array } | { ok: false; reason: "not_expired" | "not_armed" }>;
  inheritanceClear(): Promise<boolean>;
  /** Firmware update, PROTOCOL §1 "update": stream a signed image into the spare partition (`begin` → `chunk`* → `end`), then `apply` (owner's long touch, controller reboots into it on trial), then `commit` from the new image once it answers `hello`. */
  updateBegin(size: number, sha256Hex: string, version: string): Promise<UpdateBegin>;
  updateChunk(off: number, data: Uint8Array): Promise<UpdateResult>;
  updateEnd(): Promise<UpdateResult>;
  updateApply(onPending?: PendingHook, ttlMs?: number): Promise<UpdateResult>;
  updateCommit(): Promise<{ ok: true; was_trial: boolean; bought: boolean } | { ok: false; reason: UpdateReason }>;
  updateAbort(): Promise<void>;
  onEvent(cb: (e: ControllerEvent) => void): void;
  /** For the UI: is a request waiting for a touch, a long touch or the PIN rhythm right now? */
  pending(): { id: string; need: "touch" | "long_touch" | "pin" } | null;
  close(): Promise<void>;
}
