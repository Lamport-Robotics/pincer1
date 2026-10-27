/**
 * Shared message types.
 *
 * Section A mirrors docs/PROTOCOL.md §3 ("Extension ⇄ daemon") exactly: newline-delimited
 * JSON over ws://<robot>:7401. Section B is extension-internal (page ⇄ content ⇄ background
 * ⇄ popup) and never leaves the browser.
 */

// ───────────────────────── A. Extension ⇄ daemon (PROTOCOL.md §3) ─────────────────────────

export type Risk = 'low' | 'medium' | 'high' | 'block';

/** The three key families the robot holds. */
export type Chain = 'solana' | 'evm' | 'bitcoin';

/** EIP-712 typed data as dapps hand it to `eth_signTypedData_v4` (already JSON-parsed). */
export interface TypedData {
  types: Record<string, Array<{ name: string; type: string }>>;
  primaryType: string;
  domain: Record<string, unknown>;
  message: Record<string, unknown>;
}

/** `addresses` on the daemon's `connected` (and optionally `paired`) reply. */
export interface DaemonAddresses {
  solana?: string;
  /** 0x-prefixed, 20 bytes. */
  evm?: string;
  /** bech32 P2WPKH. */
  bitcoin?: string;
  /** Compressed secp256k1 pubkey of the bitcoin address, hex, if the daemon shares it. */
  bitcoinPublicKey?: string;
}

/** Extension → daemon */
export type DaemonRequest =
  | { t: 'pair'; code: string }
  | { t: 'connect'; token: string; origin: string }
  // forget: the daemon drops this token so a copy of extension storage cannot reuse it
  | { t: 'revoke'; token: string }
  // solana: `tx` is the base64 serialized (legacy or v0) transaction
  | { t: 'signTransaction'; id: string; chain: 'solana'; tx: string; origin: string }
  // evm: `tx` is the 0x-hex RLP of the UNSIGNED typed (0x02) or EIP-155 legacy transaction
  | { t: 'signTransaction'; id: string; chain: 'evm'; tx: string; chainId: number; origin: string }
  // bitcoin: `psbt` base64; `signInputs` = address → input indexes (sats-connect shape);
  // `finalize` asks the daemon to also finalize and return the raw tx hex so we can broadcast it
  | {
      t: 'signTransaction';
      id: string;
      chain: 'bitcoin';
      psbt: string;
      signInputs?: Record<string, number[]>;
      finalize: boolean;
      origin: string;
    }
  // solana: base64 message bytes
  | { t: 'signMessage'; id: string; chain: 'solana'; message: string; origin: string }
  // evm personal_sign: 0x-hex message bytes; the signer applies the EIP-191 prefix
  | { t: 'signMessage'; id: string; chain: 'evm'; message: string; origin: string }
  // evm eth_signTypedData_v4 (EIP-712)
  | { t: 'signMessage'; id: string; chain: 'evm'; typedData: TypedData; origin: string }
  // bitcoin BIP-322: plain UTF-8 message, signed with the key behind `address`
  | { t: 'signMessage'; id: string; chain: 'bitcoin'; message: string; address: string; origin: string };

/** Daemon → extension */
export type DaemonMessage =
  | { t: 'paired'; token: string; pubkey: string; addresses?: DaemonAddresses }
  | { t: 'connected'; pubkey: string; addresses?: DaemonAddresses }
  | { t: 'explain'; id: string; summary: string; risk: Risk; reasons: string[]; lines?: string[] }
  /**
   * `tx`: solana → base64 signed tx; evm → 0x-hex signed RLP; bitcoin → raw tx hex (only when
   * `finalize` was requested and the PSBT was complete). `psbt`: bitcoin signed PSBT base64.
   * `signature`: solana → base64; evm → 0x-hex 65 bytes; bitcoin → base64 BIP-322.
   */
  | { t: 'signed'; id: string; tx?: string; psbt?: string; signature?: string }
  | { t: 'denied'; id?: string; reason: string }
  // Not in §3 but tolerated: a daemon-side failure for a handshake message without an id.
  | { t: 'error'; id?: string; reason: string };

export const DEFAULT_DAEMON_URL = 'ws://pincer1.local:7401';
export const DEFAULT_RPC_URL = 'https://api.mainnet-beta.solana.com';
/** Esplora-compatible REST base used to broadcast finalized bitcoin transactions (`POST {base}/tx`). */
export const DEFAULT_BTC_API_URL = 'https://mempool.space/api';
/** Daemon ttl for a sign is 60 s; we give the whole round trip 90 s. */
export const REQUEST_TIMEOUT_MS = 90_000;

// ───────────────────────── B. Extension-internal ─────────────────────────

/** `window.postMessage` channel tag; every page↔content frame carries it. */
export const PAGE_CHANNEL = 'pincer1' as const;
export const PORT_PAGE = 'pincer1-page' as const;
export const PORT_POPUP = 'pincer1-popup' as const;

/** The dapp side of an `eth_sendTransaction` / `eth_signTransaction` param (all hex strings). */
export interface EvmTxRequest {
  from: string;
  to?: string | null;
  value?: string;
  data?: string;
  input?: string;
  gas?: string;
  gasLimit?: string;
  gasPrice?: string;
  maxFeePerGas?: string;
  maxPriorityFeePerGas?: string;
  nonce?: string;
  type?: string;
  chainId?: string;
  accessList?: Array<{ address: string; storageKeys: string[] }>;
}

export type PageMethod =
  // solana (Wallet Standard)
  | 'connect'
  | 'disconnect'
  | 'signTransaction'
  | 'signMessage'
  | 'signAndSendTransaction'
  // shared: a per-chain account for an origin (one daemon `connect` serves all chains)
  | 'connectChain'
  // evm (EIP-1193)
  | 'evm_chain'
  | 'evm_switchChain'
  | 'evm_rpc'
  | 'evm_sendTransaction'
  | 'evm_signTransaction'
  | 'evm_signMessage'
  | 'evm_signTypedData'
  // bitcoin
  | 'btc_signPsbt'
  | 'btc_signMessage';

export interface PageParams {
  connect: { silent?: boolean };
  disconnect: Record<string, never>;
  signTransaction: { tx: string; chain?: string };
  signMessage: { message: string };
  signAndSendTransaction: {
    tx: string;
    chain: string;
    options?: {
      skipPreflight?: boolean;
      preflightCommitment?: 'processed' | 'confirmed' | 'finalized';
      maxRetries?: number;
      minContextSlot?: number;
    };
  };
  connectChain: { chain: 'evm' | 'bitcoin'; silent?: boolean };
  evm_chain: Record<string, never>;
  evm_switchChain: { chainId: string };
  evm_rpc: { method: string; params: unknown[] };
  evm_sendTransaction: { tx: EvmTxRequest };
  evm_signTransaction: { tx: EvmTxRequest };
  evm_signMessage: { message: string };
  evm_signTypedData: { typedData: TypedData };
  btc_signPsbt: { psbt: string; signInputs?: Record<string, number[]>; broadcast?: boolean };
  btc_signMessage: { address: string; message: string };
}

export interface PageResults {
  connect: { pubkey: string };
  disconnect: Record<string, never>;
  signTransaction: { tx: string };
  signMessage: { signature: string };
  signAndSendTransaction: { signature: string };
  /** `address` is null for a silent call when the origin is not connected yet. */
  connectChain: { address: string | null; publicKey?: string; chainId: string };
  evm_chain: { chainId: string };
  evm_switchChain: { chainId: string };
  evm_rpc: { result: unknown };
  evm_sendTransaction: { hash: string };
  evm_signTransaction: { tx: string };
  evm_signMessage: { signature: string };
  evm_signTypedData: { signature: string };
  btc_signPsbt: { psbt: string; txid?: string };
  btc_signMessage: { signature: string };
}

export type ErrorCode =
  | 'NOT_PAIRED'
  | 'ROBOT_OFFLINE'
  | 'NOT_CONNECTED'
  | 'DENIED'
  | 'TIMEOUT'
  | 'BAD_REQUEST'
  | 'SEND_FAILED'
  | 'UNSUPPORTED_CHAIN'
  | 'UNSUPPORTED_METHOD'
  | 'RPC_ERROR'
  | 'INTERNAL';

export interface WalletError {
  code: ErrorCode;
  message: string;
  /** For RPC_ERROR: the JSON-RPC error the upstream node returned, passed through to the dapp. */
  rpc?: { code: number; data?: unknown };
}

/** Page (MAIN world) → content script (isolated world) → background. */
export interface PageRequest<M extends PageMethod = PageMethod> {
  channel: typeof PAGE_CHANNEL;
  dir: 'page→ext';
  id: string;
  method: M;
  params: PageParams[M];
}

/** Unsolicited background → page events. */
export type PageEvent = { event: 'disconnected' } | { event: 'evmChainChanged'; chainId: string };
export type PageEventName = PageEvent['event'];

/** Background → content → page. Either a reply to a request or an unsolicited event. */
export type PageReply =
  | { channel: typeof PAGE_CHANNEL; dir: 'ext→page'; id: string; ok: true; result: unknown }
  | { channel: typeof PAGE_CHANNEL; dir: 'ext→page'; id: string; ok: false; error: WalletError }
  | ({ channel: typeof PAGE_CHANNEL; dir: 'ext→page' } & PageEvent);

export type PageFrame = PageRequest | PageReply;

export function isPageRequest(v: unknown): v is PageRequest {
  if (!v || typeof v !== 'object') return false;
  const m = v as Record<string, unknown>;
  return (
    m.channel === PAGE_CHANNEL &&
    m.dir === 'page→ext' &&
    typeof m.id === 'string' &&
    typeof m.method === 'string' &&
    typeof m.params === 'object' &&
    m.params !== null
  );
}

export function isPageReply(v: unknown): v is PageReply {
  if (!v || typeof v !== 'object') return false;
  const m = v as Record<string, unknown>;
  return m.channel === PAGE_CHANNEL && m.dir === 'ext→page';
}

// ───────────────────────── Popup ⇄ background ─────────────────────────

export type SocketStatus = 'idle' | 'connecting' | 'open' | 'reconnecting';

export type RequestKind =
  | 'signTransaction'
  | 'signMessage'
  | 'signAndSendTransaction'
  | 'evm_sendTransaction'
  | 'evm_signTransaction'
  | 'evm_signMessage'
  | 'evm_signTypedData'
  | 'btc_signPsbt'
  | 'btc_signMessage';

export interface Explain {
  summary: string;
  risk: Risk;
  reasons: string[];
  /** Extra explanation lines the daemon may add (what the robot read out). */
  lines?: string[];
}

export interface PendingView {
  id: string;
  kind: RequestKind;
  chain: Chain;
  origin: string;
  startedAt: number;
  explain: Explain | null;
}

export type Outcome = 'signed' | 'denied' | 'timeout' | 'error';

export interface LogEntry {
  id: string;
  kind: RequestKind;
  chain?: Chain;
  origin: string;
  ts: number;
  outcome: Outcome;
  summary?: string;
  risk?: Risk;
  detail?: string;
}

export interface Settings {
  daemonUrl: string;
  rpcUrl: string;
  token?: string;
  pubkey?: string;
  /** Last addresses the daemon reported (persisted so the popup can show them without a dapp). */
  addresses?: DaemonAddresses;
  /** Current EVM chain (decimal id). */
  evmChainId?: number;
  /** Per-chain RPC overrides, keyed by decimal chain id. */
  evmRpcUrls?: Record<string, string>;
  btcApiUrl?: string;
  takeoverEthereum?: boolean;
  /** Sites the owner approved in the popup; any other origin's `connect` waits for an answer. */
  approvedOrigins?: string[];
  log: LogEntry[];
}

export interface StateView {
  socket: SocketStatus;
  daemonUrl: string;
  rpcUrl: string;
  paired: boolean;
  pubkey: string | null;
  addresses: DaemonAddresses;
  evmChainId: number;
  /** Effective RPC per chain (override or default), keyed by decimal chain id. */
  evmRpcUrls: Record<string, string>;
  btcApiUrl: string;
  takeoverEthereum: boolean;
  connectedOrigins: string[];
  approvedOrigins: string[];
  /** Origins whose first `connect` is waiting for Approve / Deny in the popup. */
  pendingApprovals: string[];
  pending: PendingView | null;
  log: LogEntry[];
  lastError: string | null;
}

export type PopupCommand =
  | { t: 'setDaemonUrl'; url: string }
  | { t: 'setRpcUrl'; url: string }
  | { t: 'setEvmChain'; chainId: number }
  | { t: 'setEvmRpcUrl'; chainId: number; url: string }
  | { t: 'setBtcApiUrl'; url: string }
  | { t: 'setTakeoverEthereum'; on: boolean }
  | { t: 'pair'; code: string }
  | { t: 'forget' }
  | { t: 'reconnect' }
  | { t: 'approveOrigin'; origin: string }
  | { t: 'denyOrigin'; origin: string }
  | { t: 'removeOrigin'; origin: string };

export type PopupMessage = { t: 'state'; state: StateView } | { t: 'toast'; text: string; kind: 'ok' | 'err' };

export const LOG_LIMIT = 20;
