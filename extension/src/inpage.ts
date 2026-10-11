/**
 * Runs in the page's MAIN world (declared with `"world": "MAIN"` in the manifest, at
 * document_start). Installs the three Pincer1 wallets:
 *
 *   - Solana: a Wallet Standard wallet named "Pincer1" (`registerWallet`), found by
 *     @solana/wallet-adapter and friends;
 *   - EVM: an EIP-1193 provider announced via EIP-6963 and at `window.pincer1.ethereum`
 *     (`window.ethereum` only with the opt-in takeover, see src/takeover.ts);
 *   - Bitcoin: `window.pincer1.bitcoin` plus a WBIP004 entry in `window.btc_providers`.
 *
 * None of them holds a key: every request is framed over window.postMessage to the content
 * script, which relays it to the background, which relays it to the robot over one socket.
 */
import {
  registerWallet,
  StandardConnect,
  StandardDisconnect,
  StandardEvents,
  type IdentifierArray,
  type StandardConnectFeature,
  type StandardConnectMethod,
  type StandardDisconnectFeature,
  type StandardDisconnectMethod,
  type StandardEventsFeature,
  type StandardEventsListeners,
  type StandardEventsNames,
  type StandardEventsOnMethod,
  type Wallet,
  type WalletAccount,
  type WalletIcon,
} from '@wallet-standard/core';
import {
  SolanaSignAndSendTransaction,
  SolanaSignMessage,
  SolanaSignTransaction,
  type SolanaSignAndSendTransactionFeature,
  type SolanaSignAndSendTransactionMethod,
  type SolanaSignMessageFeature,
  type SolanaSignMessageMethod,
  type SolanaSignTransactionFeature,
  type SolanaSignTransactionMethod,
} from '@solana/wallet-standard-features';
import { SOLANA_DEVNET_CHAIN, SOLANA_LOCALNET_CHAIN, SOLANA_MAINNET_CHAIN } from '@solana/wallet-standard-chains';
import { PageTransport, PincerError } from './lib/pageTransport';
import { fromBase58, fromBase64, toBase64 } from './lib/bytes';
import { ICON_DATA_URI } from './generated/icon';
import { Pincer1EthereumProvider, announceEip6963, defineWindowEthereum } from './evm/provider';
import { Pincer1BitcoinProvider, registerBtcProvider } from './bitcoin/provider';

const CHAINS = [SOLANA_MAINNET_CHAIN, SOLANA_DEVNET_CHAIN, SOLANA_LOCALNET_CHAIN] as const;

type PincerFeatures = StandardConnectFeature &
  StandardDisconnectFeature &
  StandardEventsFeature &
  SolanaSignTransactionFeature &
  SolanaSignAndSendTransactionFeature &
  SolanaSignMessageFeature;

class PincerAccount implements WalletAccount {
  readonly chains: IdentifierArray = CHAINS;
  readonly features: IdentifierArray = [SolanaSignTransaction, SolanaSignAndSendTransaction, SolanaSignMessage];
  readonly label = 'Pincer1';
  readonly icon = ICON_DATA_URI as WalletIcon;
  readonly publicKey: Uint8Array;
  constructor(readonly address: string) {
    this.publicKey = fromBase58(address);
    Object.freeze(this);
  }
}

export class PincerWallet implements Wallet {
  readonly version = '1.0.0' as const;
  readonly name = 'Pincer1' as const;
  readonly icon = ICON_DATA_URI as WalletIcon;
  readonly chains = CHAINS;

  #account: PincerAccount | null = null;
  readonly #listeners = new Map<StandardEventsNames, Set<(...args: unknown[]) => void>>();

  constructor(private readonly transport: PageTransport) {
    transport.onEvent((name) => {
      if (name === 'disconnected' && this.#account) {
        this.#account = null;
        this.#emit('change', { accounts: this.accounts });
      }
    });
  }

  get accounts(): readonly WalletAccount[] {
    return this.#account ? [this.#account] : [];
  }

  get features(): PincerFeatures {
    return {
      [StandardConnect]: { version: '1.0.0', connect: this.#connect },
      [StandardDisconnect]: { version: '1.0.0', disconnect: this.#disconnect },
      [StandardEvents]: { version: '1.0.0', on: this.#on },
      [SolanaSignTransaction]: {
        version: '1.0.0',
        supportedTransactionVersions: ['legacy', 0],
        signTransaction: this.#signTransaction,
      },
      [SolanaSignAndSendTransaction]: {
        version: '1.0.0',
        supportedTransactionVersions: ['legacy', 0],
        signAndSendTransaction: this.#signAndSendTransaction,
      },
      [SolanaSignMessage]: { version: '1.1.0', signMessage: this.#signMessage },
    };
  }

  #emit<E extends StandardEventsNames>(event: E, ...args: Parameters<StandardEventsListeners[E]>): void {
    for (const l of this.#listeners.get(event) ?? []) {
      try {
        l(...args);
      } catch {
        /* listener errors must not break the wallet */
      }
    }
  }

  #on: StandardEventsOnMethod = (event, listener) => {
    let set = this.#listeners.get(event);
    if (!set) this.#listeners.set(event, (set = new Set()));
    set.add(listener as (...args: unknown[]) => void);
    return () => {
      set.delete(listener as (...args: unknown[]) => void);
    };
  };

  #connect: StandardConnectMethod = async (input) => {
    if (this.#account && input?.silent) return { accounts: this.accounts };
    const { pubkey } = await this.transport.request('connect', { silent: input?.silent });
    if (!this.#account || this.#account.address !== pubkey) {
      this.#account = new PincerAccount(pubkey);
      this.#emit('change', { accounts: this.accounts });
    }
    return { accounts: this.accounts };
  };

  #disconnect: StandardDisconnectMethod = async () => {
    try {
      await this.transport.request('disconnect', {});
    } finally {
      if (this.#account) {
        this.#account = null;
        this.#emit('change', { accounts: this.accounts });
      }
    }
  };

  #assertAccount(account: WalletAccount): void {
    if (!this.#account) throw new PincerError('NOT_CONNECTED', 'Pincer1 is not connected');
    if (account.address !== this.#account.address) {
      throw new PincerError('BAD_REQUEST', 'account does not belong to Pincer1');
    }
  }

  #assertChain(chain: string | undefined): void {
    if (chain !== undefined && !(CHAINS as readonly string[]).includes(chain)) {
      throw new PincerError('BAD_REQUEST', `unsupported chain ${chain}`);
    }
  }

  #signTransaction: SolanaSignTransactionMethod = async (...inputs) => {
    const out = [];
    for (const input of inputs) {
      this.#assertAccount(input.account);
      this.#assertChain(input.chain);
      const { tx } = await this.transport.request('signTransaction', {
        tx: toBase64(input.transaction),
        chain: input.chain,
      });
      out.push({ signedTransaction: fromBase64(tx) });
    }
    return out;
  };

  #signAndSendTransaction: SolanaSignAndSendTransactionMethod = async (...inputs) => {
    const out = [];
    for (const input of inputs) {
      this.#assertAccount(input.account);
      this.#assertChain(input.chain);
      const { signature } = await this.transport.request('signAndSendTransaction', {
        tx: toBase64(input.transaction),
        chain: input.chain,
        options: input.options && {
          skipPreflight: input.options.skipPreflight,
          preflightCommitment: input.options.preflightCommitment,
          maxRetries: input.options.maxRetries,
          minContextSlot: input.options.minContextSlot,
        },
      });
      out.push({ signature: fromBase58(signature) });
    }
    return out;
  };

  #signMessage: SolanaSignMessageMethod = async (...inputs) => {
    const out = [];
    for (const input of inputs) {
      this.#assertAccount(input.account);
      const { signature } = await this.transport.request('signMessage', {
        message: toBase64(input.message),
      });
      out.push({ signedMessage: input.message, signature: fromBase64(signature) });
    }
    return out;
  };
}

/** What `window.pincer1` holds. `request` forwards to bitcoin so sats-connect's id lookup works. */
export interface Pincer1Namespace {
  readonly ethereum: Pincer1EthereumProvider;
  readonly bitcoin: Pincer1BitcoinProvider;
  readonly request: Pincer1BitcoinProvider['request'];
  readonly version: string;
}

interface InstallTarget {
  __pincer1Installed?: boolean;
  __pincer1TakeoverEthereum?: boolean;
  pincer1?: Pincer1Namespace;
  btc_providers?: unknown;
  addEventListener(type: string, listener: (ev: unknown) => void): void;
  dispatchEvent(ev: unknown): boolean;
}

/** Builds and exposes all three wallets on a window-like object. Exported for tests. */
export function installWallets(win: InstallTarget, transport: PageTransport): Pincer1Namespace {
  const solana = new PincerWallet(transport);
  registerWallet(solana);

  const ethereum = new Pincer1EthereumProvider(transport);
  const bitcoin = new Pincer1BitcoinProvider(transport);
  const ns: Pincer1Namespace = Object.freeze({
    ethereum,
    bitcoin,
    request: bitcoin.request.bind(bitcoin),
    version: '0.2.0',
  });
  try {
    Object.defineProperty(win, 'pincer1', { value: ns, writable: false, configurable: false, enumerable: true });
  } catch {
    win.pincer1 = ns;
  }
  announceEip6963(win, ethereum);
  registerBtcProvider(win);
  // The opt-in takeover script (src/takeover.ts) may have run before us and left a flag.
  if (win.__pincer1TakeoverEthereum) defineWindowEthereum(win, ethereum);
  return ns;
}

function install(): void {
  if (typeof window === 'undefined') return;
  // Opaque origins (sandboxed frames, about:blank) cannot be origin-checked; stay out of them.
  if (!window.location.origin || window.location.origin === 'null') return;
  const w = window as unknown as InstallTarget;
  if (w.__pincer1Installed) return;
  w.__pincer1Installed = true;
  installWallets(w, new PageTransport(window));
}

install();
