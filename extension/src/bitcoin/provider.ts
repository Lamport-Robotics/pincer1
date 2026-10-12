/**
 * Bitcoin wallet surface at `window.pincer1.bitcoin`.
 *
 * Two entry styles, because that is what dapps actually probe:
 *  - direct methods (`requestAccounts`, `signPsbt`, `signMessage`, `getNetwork`) in the shape
 *    the Xverse / Leather / Unisat injected objects share, which most "connect wallet" code and
 *    sats-connect's legacy `getAddress` path end up calling;
 *  - `request(method, params)` returning a JSON-RPC-style `{ result } | { error }`, the
 *    sats-connect v2 / WBIP wallet-RPC surface (`getAccounts`, `signPsbt`, `signMessage`,
 *    `wallet_getNetwork`, `wallet_connect`).
 *
 * Discovery: a WBIP004 entry is pushed onto `window.btc_providers` with id `pincer1`, so wallet
 * pickers (sats-connect `getProviders()`, Xverse-style selectors) list Pincer1. sats-connect
 * resolves the provider object from the id by walking `window.<id>`, hence `window.pincer1`
 * itself forwards `request()` to this provider (see `src/inpage.ts`).
 *
 * One address, P2WPKH, mainnet. No key here: everything goes to the robot through the page
 * transport and comes back as a signed PSBT / BIP-322 signature.
 */
import { PageTransport, PincerError } from '../lib/pageTransport';
import type { WalletError } from '../protocol';
import { ICON_DATA_URI } from '../generated/icon';

export interface BitcoinAccount {
  address: string;
  publicKey?: string;
  purpose: 'payment';
  addressType: 'p2wpkh';
}

export interface SignPsbtOptions {
  /** address → input indexes to sign (sats-connect shape). Omitted = every input we own. */
  signInputs?: Record<string, number[]>;
  /** Finalize on the robot and broadcast through the API URL set in the popup. */
  broadcast?: boolean;
}

export interface SignPsbtResult {
  psbt: string;
  txid?: string;
}

/** sats-connect `RpcErrorCode` values. */
export const BTC_RPC_ERRORS = {
  USER_REJECTION: -32000,
  ACCESS_DENIED: -32002,
  METHOD_NOT_SUPPORTED: -32601,
  INVALID_PARAMS: -32602,
  INTERNAL: -32603,
} as const;

export class BitcoinProviderError extends Error {
  constructor(
    readonly code: number,
    message: string,
  ) {
    super(message);
    this.name = 'BitcoinProviderError';
  }
}

export function toBitcoinError(e: unknown): BitcoinProviderError {
  if (e instanceof BitcoinProviderError) return e;
  const code = (e as Partial<WalletError>)?.code;
  const message = (e as Error)?.message ?? String(e);
  switch (code) {
    case 'DENIED':
      return new BitcoinProviderError(BTC_RPC_ERRORS.USER_REJECTION, message);
    case 'NOT_CONNECTED':
    case 'NOT_PAIRED':
      return new BitcoinProviderError(BTC_RPC_ERRORS.ACCESS_DENIED, message);
    case 'UNSUPPORTED_METHOD':
      return new BitcoinProviderError(BTC_RPC_ERRORS.METHOD_NOT_SUPPORTED, message);
    case 'BAD_REQUEST':
      return new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, message);
    default:
      return new BitcoinProviderError(BTC_RPC_ERRORS.INTERNAL, message);
  }
}

export type BtcRpcResponse =
  | { jsonrpc: '2.0'; id: string; result: unknown }
  | { jsonrpc: '2.0'; id: string; error: { code: number; message: string } };

const NETWORK = Object.freeze({ bitcoin: Object.freeze({ name: 'Mainnet' }), stacks: Object.freeze({ name: 'Mainnet' }) });

type Listener = (...args: unknown[]) => void;

export class Pincer1BitcoinProvider {
  readonly isPincer1 = true as const;
  #account: BitcoinAccount | null = null;
  readonly #listeners = new Map<string, Set<Listener>>();

  constructor(private readonly transport: PageTransport) {
    transport.onEvent((name) => {
      if (name === 'disconnected' && this.#account) {
        this.#account = null;
        this.#emit('accountsChanged', []);
      }
    });
  }

  on(event: string, listener: Listener): () => void {
    let set = this.#listeners.get(event);
    if (!set) this.#listeners.set(event, (set = new Set()));
    set.add(listener);
    return () => void set!.delete(listener);
  }
  removeListener(event: string, listener: Listener): void {
    this.#listeners.get(event)?.delete(listener);
  }
  #emit(event: string, ...args: unknown[]): void {
    for (const l of this.#listeners.get(event) ?? []) {
      try {
        l(...args);
      } catch {
        /* ignore */
      }
    }
  }

  /** Asks the robot (through its daemon) for this origin; the daemon decides whether to connect. */
  async requestAccounts(): Promise<BitcoinAccount[]> {
    try {
      const { address, publicKey } = await this.transport.request('connectChain', { chain: 'bitcoin' });
      if (!address) throw new PincerError('NOT_CONNECTED', 'the robot exposes no bitcoin account');
      return this.#setAccount(address, publicKey);
    } catch (e) {
      throw toBitcoinError(e);
    }
  }

  /** Accounts without prompting: only if this origin is already connected. */
  async getAccounts(): Promise<BitcoinAccount[]> {
    if (this.#account) return [this.#account];
    try {
      const { address, publicKey } = await this.transport.request('connectChain', { chain: 'bitcoin', silent: true });
      return address ? this.#setAccount(address, publicKey) : [];
    } catch (e) {
      throw toBitcoinError(e);
    }
  }

  #setAccount(address: string, publicKey?: string): BitcoinAccount[] {
    const changed = this.#account?.address !== address;
    this.#account = Object.freeze({ address, publicKey, purpose: 'payment', addressType: 'p2wpkh' });
    if (changed) this.#emit('accountsChanged', [this.#account]);
    return [this.#account];
  }

  getNetwork(): Promise<typeof NETWORK> {
    return Promise.resolve(NETWORK);
  }

  async signPsbt(psbtBase64: string, options: SignPsbtOptions = {}): Promise<SignPsbtResult> {
    if (typeof psbtBase64 !== 'string' || !/^[A-Za-z0-9+/]+=*$/.test(psbtBase64)) {
      throw new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, 'psbt must be base64');
    }
    if (!this.#account) throw new BitcoinProviderError(BTC_RPC_ERRORS.ACCESS_DENIED, 'call requestAccounts first');
    if (options.signInputs !== undefined) {
      for (const [addr, idx] of Object.entries(options.signInputs)) {
        if (addr !== this.#account.address) {
          throw new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, `signInputs names an address Pincer1 does not hold: ${addr}`);
        }
        if (!Array.isArray(idx) || idx.some((i) => !Number.isInteger(i) || i < 0)) {
          throw new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, 'signInputs indexes must be non-negative integers');
        }
      }
    }
    try {
      return await this.transport.request('btc_signPsbt', {
        psbt: psbtBase64,
        signInputs: options.signInputs,
        broadcast: options.broadcast === true,
      });
    } catch (e) {
      throw toBitcoinError(e);
    }
  }

  /** BIP-322 simple signature over a UTF-8 message, base64. */
  async signMessage(address: string, message: string): Promise<string> {
    if (!this.#account) throw new BitcoinProviderError(BTC_RPC_ERRORS.ACCESS_DENIED, 'call requestAccounts first');
    if (address !== this.#account.address) {
      throw new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, 'address is not the Pincer1 account');
    }
    if (typeof message !== 'string') throw new BitcoinProviderError(BTC_RPC_ERRORS.INVALID_PARAMS, 'message must be a string');
    try {
      const { signature } = await this.transport.request('btc_signMessage', { address, message });
      return signature;
    } catch (e) {
      throw toBitcoinError(e);
    }
  }

  /** sats-connect v2 / WBIP wallet-RPC surface. Never throws: errors come back in the envelope. */
  async request(method: string, params?: unknown, id: string = `p${Date.now().toString(36)}`): Promise<BtcRpcResponse> {
    const ok = (result: unknown): BtcRpcResponse => ({ jsonrpc: '2.0', id, result });
    try {
      const p = (params ?? {}) as Record<string, unknown>;
      switch (method) {
        case 'wallet_connect':
        case 'wallet_requestPermissions':
        case 'getAddresses':
        case 'requestAccounts': {
          const accounts = await this.requestAccounts();
          return ok({ addresses: accounts.map(toAddressEntry) });
        }
        case 'getAccounts': {
          const accounts = await this.getAccounts();
          return ok(accounts.map(toAddressEntry));
        }
        case 'getNetwork':
        case 'wallet_getNetwork':
          return ok(await this.getNetwork());
        case 'signPsbt': {
          const r = await this.signPsbt(p.psbt as string, {
            signInputs: p.signInputs as Record<string, number[]> | undefined,
            broadcast: p.broadcast === true,
          });
          return ok(r);
        }
        case 'signMessage': {
          const address = (p.address as string | undefined) ?? this.#account?.address ?? '';
          const signature = await this.signMessage(address, p.message as string);
          return ok({ signature, address });
        }
        case 'wallet_disconnect':
        case 'wallet_renouncePermissions':
          await this.transport.request('disconnect', {});
          if (this.#account) {
            this.#account = null;
            this.#emit('accountsChanged', []);
          }
          return ok(null);
        default:
          throw new BitcoinProviderError(BTC_RPC_ERRORS.METHOD_NOT_SUPPORTED, `${method} is not supported by Pincer1`);
      }
    } catch (e) {
      const err = toBitcoinError(e);
      return { jsonrpc: '2.0', id, error: { code: err.code, message: err.message } };
    }
  }
}

function toAddressEntry(a: BitcoinAccount): Record<string, unknown> {
  return { address: a.address, publicKey: a.publicKey, purpose: a.purpose, addressType: a.addressType };
}

/** WBIP004 provider entry. `id` is the path on `window` that resolves to an object with `request()`. */
export const BTC_PROVIDER_ENTRY = Object.freeze({
  id: 'pincer1',
  name: 'Pincer1',
  icon: ICON_DATA_URI,
  webUrl: 'https://lamportrobotics.com',
  chromeWebStoreUrl: undefined as string | undefined,
  methods: ['getAccounts', 'getAddresses', 'requestAccounts', 'signPsbt', 'signMessage', 'wallet_getNetwork', 'wallet_connect'],
});

/** Push the WBIP004 entry onto `window.btc_providers` (creating the array if missing). */
export function registerBtcProvider(win: { btc_providers?: unknown }): void {
  const list = Array.isArray(win.btc_providers) ? (win.btc_providers as unknown[]) : (win.btc_providers = []);
  if (!list.some((p) => (p as { id?: string })?.id === BTC_PROVIDER_ENTRY.id)) list.push({ ...BTC_PROVIDER_ENTRY });
}
