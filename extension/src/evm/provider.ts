/**
 * EIP-1193 provider for the Pincer1 robot key, announced to dapps through EIP-6963
 * (`eip6963:announceProvider`) and exposed as `window.pincer1.ethereum`. It never overwrites
 * `window.ethereum` on its own — see `src/takeover.ts` for the opt-in.
 *
 * Holds no key and talks to no node itself: every call is framed to the content script and
 * handled by the background, which signs through the robot and reads/broadcasts through the
 * RPC the user set in the popup.
 */
import { hexToNumber, isAddress, isHex, stringToHex, type Hex } from 'viem';
import { EVM_READONLY_METHODS, chainIdHex, parseChainId } from './chains';
import { PageTransport, PincerError } from '../lib/pageTransport';
import type { TypedData, WalletError } from '../protocol';
import { ICON_DATA_URI } from '../generated/icon';

/** EIP-1193 / EIP-1474 error codes dapps switch on. */
export const RPC_ERRORS = {
  USER_REJECTED: 4001,
  UNAUTHORIZED: 4100,
  UNSUPPORTED_METHOD: 4200,
  DISCONNECTED: 4900,
  CHAIN_DISCONNECTED: 4901,
  UNRECOGNIZED_CHAIN: 4902,
  INVALID_PARAMS: -32602,
  INTERNAL: -32603,
} as const;

export class ProviderRpcError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly data?: unknown,
  ) {
    super(message);
    this.name = 'ProviderRpcError';
  }
}

/** Map an extension-internal failure onto the EIP-1193 code a dapp expects. */
export function toProviderError(e: unknown): ProviderRpcError {
  if (e instanceof ProviderRpcError) return e;
  const code = (e as Partial<WalletError>)?.code;
  const message = (e as Error)?.message ?? String(e);
  switch (code) {
    case 'DENIED':
      return new ProviderRpcError(RPC_ERRORS.USER_REJECTED, message);
    case 'NOT_CONNECTED':
    case 'NOT_PAIRED':
      return new ProviderRpcError(RPC_ERRORS.UNAUTHORIZED, message);
    case 'UNSUPPORTED_METHOD':
      return new ProviderRpcError(RPC_ERRORS.UNSUPPORTED_METHOD, message);
    case 'UNSUPPORTED_CHAIN':
      return new ProviderRpcError(RPC_ERRORS.UNRECOGNIZED_CHAIN, message);
    case 'ROBOT_OFFLINE':
      return new ProviderRpcError(RPC_ERRORS.DISCONNECTED, message);
    case 'BAD_REQUEST':
      return new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, message);
    case 'RPC_ERROR': {
      const rpc = (e as WalletError).rpc;
      return new ProviderRpcError(rpc?.code ?? RPC_ERRORS.INTERNAL, message, rpc?.data);
    }
    default:
      return new ProviderRpcError(RPC_ERRORS.INTERNAL, message);
  }
}

export interface RequestArguments {
  method: string;
  params?: unknown[] | Record<string, unknown>;
}

export interface EIP6963ProviderInfo {
  uuid: string;
  name: string;
  icon: string;
  rdns: string;
}

export const PROVIDER_INFO: Omit<EIP6963ProviderInfo, 'uuid'> = {
  name: 'Pincer1',
  icon: ICON_DATA_URI,
  rdns: 'com.lamportrobotics.pincer1',
};

type Listener = (...args: unknown[]) => void;

/** The bits of `window` the provider needs; a fake stands in under vitest. */
export interface EventTargetLike {
  addEventListener(type: string, listener: (ev: unknown) => void): void;
  dispatchEvent(ev: unknown): boolean;
}

export class Pincer1EthereumProvider {
  /** Lets a dapp tell us apart; we deliberately set no `isMetaMask`. */
  readonly isPincer1 = true as const;

  #account: string | null = null;
  #chainId: string | null = null;
  #connected = false;
  readonly #listeners = new Map<string, Set<Listener>>();

  constructor(private readonly transport: PageTransport) {
    transport.onEvent((name, payload) => {
      if (name === 'disconnected') {
        if (this.#account) {
          this.#account = null;
          this.#emit('accountsChanged', []);
        }
        if (this.#connected) {
          this.#connected = false;
          this.#emit('disconnect', new ProviderRpcError(RPC_ERRORS.DISCONNECTED, 'Pincer1 was unpaired'));
        }
      } else if (name === 'evmChainChanged' && payload && 'chainId' in payload) {
        this.#setChain(payload.chainId);
      }
    });
  }

  // ── legacy-ish properties many dapps still read ──
  get chainId(): string | null {
    return this.#chainId;
  }
  get networkVersion(): string | null {
    return this.#chainId ? String(hexToNumber(this.#chainId as Hex)) : null;
  }
  get selectedAddress(): string | null {
    return this.#account;
  }
  isConnected(): boolean {
    return this.#connected;
  }
  /** Pre-1193 `ethereum.enable()`. */
  enable(): Promise<string[]> {
    return this.request({ method: 'eth_requestAccounts' }) as Promise<string[]>;
  }

  // ── events ──
  on(event: string, listener: Listener): this {
    let set = this.#listeners.get(event);
    if (!set) this.#listeners.set(event, (set = new Set()));
    set.add(listener);
    return this;
  }
  addListener(event: string, listener: Listener): this {
    return this.on(event, listener);
  }
  once(event: string, listener: Listener): this {
    const wrap: Listener = (...args) => {
      this.removeListener(event, wrap);
      listener(...args);
    };
    return this.on(event, wrap);
  }
  removeListener(event: string, listener: Listener): this {
    this.#listeners.get(event)?.delete(listener);
    return this;
  }
  off(event: string, listener: Listener): this {
    return this.removeListener(event, listener);
  }
  removeAllListeners(event?: string): this {
    if (event) this.#listeners.delete(event);
    else this.#listeners.clear();
    return this;
  }
  #emit(event: string, ...args: unknown[]): void {
    for (const l of this.#listeners.get(event) ?? []) {
      try {
        l(...args);
      } catch {
        /* a dapp listener must not break the wallet */
      }
    }
  }

  #setChain(chainId: string): void {
    if (this.#chainId === chainId) return;
    const first = this.#chainId === null;
    this.#chainId = chainId;
    if (!first) this.#emit('chainChanged', chainId);
  }

  async #ensureChain(): Promise<string> {
    if (this.#chainId) return this.#chainId;
    const { chainId } = await this.transport.request('evm_chain', {});
    this.#setChain(chainId);
    return chainId;
  }

  #setAccount(address: string | null, chainId: string): void {
    const changed = address !== this.#account;
    this.#account = address;
    if (!this.#connected && address) {
      this.#connected = true;
      this.#emit('connect', { chainId });
    }
    if (changed) this.#emit('accountsChanged', address ? [address] : []);
  }

  // ── the one entry point ──
  async request(args: RequestArguments): Promise<unknown> {
    if (!args || typeof args !== 'object' || typeof args.method !== 'string') {
      throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'request({ method, params }) expected');
    }
    const params = Array.isArray(args.params) ? args.params : args.params === undefined ? [] : [args.params];
    try {
      return await this.#dispatch(args.method, params);
    } catch (e) {
      throw toProviderError(e);
    }
  }

  async #dispatch(method: string, params: unknown[]): Promise<unknown> {
    switch (method) {
      case 'eth_requestAccounts':
      case 'wallet_requestPermissions': {
        const { address, chainId } = await this.transport.request('connectChain', { chain: 'evm' });
        if (!address) throw new PincerError('NOT_CONNECTED', 'the robot exposes no EVM account');
        this.#setChain(chainId);
        this.#setAccount(address, chainId);
        return method === 'eth_requestAccounts'
          ? [address]
          : [{ parentCapability: 'eth_accounts', caveats: [{ type: 'restrictReturnedAccounts', value: [address] }] }];
      }
      case 'eth_accounts': {
        if (this.#account) return [this.#account];
        const { address, chainId } = await this.transport.request('connectChain', { chain: 'evm', silent: true });
        this.#setChain(chainId);
        if (address) this.#setAccount(address, chainId);
        return address ? [address] : [];
      }
      case 'wallet_getPermissions':
        return this.#account ? [{ parentCapability: 'eth_accounts' }] : [];
      case 'wallet_revokePermissions': {
        await this.transport.request('disconnect', {});
        this.#setAccount(null, this.#chainId ?? '0x1');
        return null;
      }
      case 'eth_chainId':
        return this.#ensureChain();
      case 'net_version':
        return String(hexToNumber((await this.#ensureChain()) as Hex));
      case 'wallet_switchEthereumChain':
      case 'wallet_addEthereumChain': {
        const p = params[0] as { chainId?: unknown } | undefined;
        const id = parseChainId(p?.chainId);
        if (!Number.isFinite(id)) throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'chainId must be a 0x hex string');
        // addEthereumChain for a chain we already know is just a switch; unknown chains are
        // not added (the daemon owns the chain list) and come back as 4902 from the background.
        const { chainId } = await this.transport.request('evm_switchChain', { chainId: chainIdHex(id) });
        this.#setChain(chainId);
        return null;
      }
      case 'eth_sendTransaction': {
        const tx = this.#txParam(params);
        const { hash } = await this.transport.request('evm_sendTransaction', { tx });
        return hash;
      }
      case 'eth_signTransaction': {
        const tx = this.#txParam(params);
        const { tx: signed } = await this.transport.request('evm_signTransaction', { tx });
        return signed;
      }
      case 'personal_sign': {
        // MetaMask order is [data, address]; some older dapps send [address, data].
        const [a, b] = params as [unknown, unknown];
        const data = typeof a === 'string' && isAddress(a, { strict: false }) && typeof b === 'string' ? b : a;
        const message = this.#messageHex(data);
        const { signature } = await this.transport.request('evm_signMessage', { message });
        return signature;
      }
      case 'eth_signTypedData_v4':
      case 'eth_signTypedData_v3': {
        const [, raw] = params as [unknown, unknown];
        const typedData = this.#typedData(raw);
        const { signature } = await this.transport.request('evm_signTypedData', { typedData });
        return signature;
      }
      case 'eth_sign':
      case 'eth_signTypedData': // v1 (array form): legacy and phishing-prone, refused on purpose
      case 'eth_decrypt':
      case 'eth_getEncryptionPublicKey':
      case 'wallet_watchAsset':
      case 'wallet_scanQRCode':
        throw new ProviderRpcError(RPC_ERRORS.UNSUPPORTED_METHOD, `${method} is not supported by Pincer1`);
      default: {
        if (!EVM_READONLY_METHODS.has(method)) {
          throw new ProviderRpcError(RPC_ERRORS.UNSUPPORTED_METHOD, `${method} is not supported by Pincer1`);
        }
        const { result } = await this.transport.request('evm_rpc', { method, params });
        return result;
      }
    }
  }

  #txParam(params: unknown[]): import('../protocol').EvmTxRequest {
    const tx = params[0];
    if (!tx || typeof tx !== 'object') throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'transaction object expected');
    const t = tx as import('../protocol').EvmTxRequest;
    if (!this.#account) throw new ProviderRpcError(RPC_ERRORS.UNAUTHORIZED, 'call eth_requestAccounts first');
    if (typeof t.from !== 'string' || t.from.toLowerCase() !== this.#account.toLowerCase()) {
      throw new ProviderRpcError(RPC_ERRORS.UNAUTHORIZED, 'from is not the connected Pincer1 account');
    }
    return t;
  }

  /** personal_sign data: hex bytes as-is, anything else UTF-8 encoded. */
  #messageHex(data: unknown): Hex {
    if (typeof data !== 'string') throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'message must be a string');
    if (!this.#account) throw new ProviderRpcError(RPC_ERRORS.UNAUTHORIZED, 'call eth_requestAccounts first');
    if (isHex(data, { strict: true }) && data.length % 2 === 0) return data;
    return stringToHex(data);
  }

  #typedData(raw: unknown): TypedData {
    if (!this.#account) throw new ProviderRpcError(RPC_ERRORS.UNAUTHORIZED, 'call eth_requestAccounts first');
    let v: unknown = raw;
    if (typeof raw === 'string') {
      try {
        v = JSON.parse(raw);
      } catch {
        throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'typed data is not valid JSON');
      }
    }
    const td = v as Partial<TypedData> | null;
    if (!td || typeof td !== 'object' || !td.types || !td.primaryType || !td.domain || !td.message) {
      throw new ProviderRpcError(RPC_ERRORS.INVALID_PARAMS, 'typed data needs types, primaryType, domain and message');
    }
    return { types: td.types, primaryType: td.primaryType, domain: td.domain, message: td.message };
  }
}

/**
 * EIP-6963: announce on install and whenever a dapp asks (`eip6963:requestProvider`).
 * Returns the info object (the uuid is fresh per page load, as the spec wants).
 */
export function announceEip6963(
  target: EventTargetLike,
  provider: Pincer1EthereumProvider,
  uuid: string = typeof crypto !== 'undefined' && 'randomUUID' in crypto ? crypto.randomUUID() : `pincer1-${Date.now()}`,
): EIP6963ProviderInfo {
  const info: EIP6963ProviderInfo = Object.freeze({ uuid, ...PROVIDER_INFO });
  const announce = (): void => {
    target.dispatchEvent(new CustomEvent('eip6963:announceProvider', { detail: Object.freeze({ info, provider }) }));
  };
  target.addEventListener('eip6963:requestProvider', announce);
  announce();
  return info;
}

/** Defines `window.ethereum` (the opt-in takeover); silently gives up if another wallet locked it. */
export function defineWindowEthereum(win: object, provider: Pincer1EthereumProvider): boolean {
  try {
    Object.defineProperty(win, 'ethereum', { value: provider, writable: true, configurable: true, enumerable: true });
    return true;
  } catch {
    return false;
  }
}
