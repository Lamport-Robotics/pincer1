/**
 * Background core: owns the one WebSocket to pincerd, does `pair` / `connect` (PROTOCOL.md §3),
 * forwards sign requests for all three key families (solana, evm, bitcoin), surfaces `explain`
 * events, keeps the request log.
 *
 * Everything with a side effect (socket, storage, badge, RPC calls, timers) is injected so the
 * whole state machine runs under vitest against a mock socket and a fake RPC.
 */
import {
  DEFAULT_BTC_API_URL,
  DEFAULT_DAEMON_URL,
  DEFAULT_RPC_URL,
  LOG_LIMIT,
  REQUEST_TIMEOUT_MS,
  type Chain,
  type DaemonAddresses,
  type DaemonMessage,
  type DaemonRequest,
  type Explain,
  type LogEntry,
  type PageEvent,
  type PageEventName,
  type PageMethod,
  type PageParams,
  type PageRequest,
  type PageResults,
  type RequestKind,
  type Settings,
  type SocketStatus,
  type StateView,
  type TypedData,
  type WalletError,
} from '../protocol';
import { fromBase64, newId } from '../lib/bytes';
import { RpcError } from '../lib/jsonRpc';
import { DEFAULT_EVM_CHAIN_ID, EVM_CHAINS, EVM_READONLY_METHODS, chainIdHex, findEvmChain, parseChainId } from '../evm/chains';
import { EvmTxError, checkSignedEvmTransaction, prepareEvmTransaction } from '../evm/tx';

export interface SocketLike {
  send(data: string): void;
  close(): void;
  onopen: (() => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onclose: (() => void) | null;
  onerror: (() => void) | null;
}

export interface StorageLike {
  get(): Promise<Partial<Settings>>;
  set(patch: Partial<Settings>): Promise<void>;
}

export type SendOptions = PageParams['signAndSendTransaction']['options'];

export interface CoreDeps {
  createSocket(url: string): SocketLike;
  storage: StorageLike;
  setBadge(text: string): void;
  /** Broadcast the signed solana tx; resolves with the base58 signature. */
  sendRawTransaction(rpcUrl: string, signedTx: Uint8Array, options: SendOptions): Promise<string>;
  /** JSON-RPC call against an EVM node. Throws `RpcError` for node-side errors. */
  jsonRpc?(url: string, method: string, params: unknown[]): Promise<unknown>;
  /** `POST {api}/tx` with a raw bitcoin tx; resolves with the txid. */
  broadcastBitcoin?(apiUrl: string, txHex: string): Promise<string>;
  /** Called when the "take over window.ethereum" toggle changes (registers the MAIN-world script). */
  applyTakeover?(on: boolean): Promise<void>;
  now?: () => number;
  setTimeout?: (fn: () => void, ms: number) => unknown;
  clearTimeout?: (handle: unknown) => void;
  requestTimeoutMs?: number;
  handshakeTimeoutMs?: number;
  openTimeoutMs?: number;
  maxBackoffMs?: number;
}

export class CoreError extends Error implements WalletError {
  constructor(
    public readonly code: WalletError['code'],
    message: string,
    public readonly rpc?: WalletError['rpc'],
  ) {
    super(message);
    this.name = 'CoreError';
  }
  toJSON(): WalletError {
    return this.rpc ? { code: this.code, message: this.message, rpc: this.rpc } : { code: this.code, message: this.message };
  }
}

interface Pending {
  id: string;
  kind: RequestKind;
  chain: Chain;
  origin: string;
  startedAt: number;
  explain: Explain | null;
  resolve: (m: DaemonMessage & { t: 'signed' }) => void;
  reject: (e: CoreError) => void;
  timer: unknown;
}

interface Expectation {
  t: 'paired' | 'connected';
  resolve: (m: DaemonMessage) => void;
  reject: (e: CoreError) => void;
  timer: unknown;
}

const RPC_BY_CHAIN: Record<string, string | null> = {
  'solana:mainnet': null, // configurable
  'solana:devnet': 'https://api.devnet.solana.com',
  'solana:localnet': 'http://127.0.0.1:8899',
};

const KIND_CHAIN: Record<RequestKind, Chain> = {
  signTransaction: 'solana',
  signMessage: 'solana',
  signAndSendTransaction: 'solana',
  evm_sendTransaction: 'evm',
  evm_signTransaction: 'evm',
  evm_signMessage: 'evm',
  evm_signTypedData: 'evm',
  btc_signPsbt: 'bitcoin',
  btc_signMessage: 'bitcoin',
};

export class BackgroundCore {
  private settings: Settings = { daemonUrl: DEFAULT_DAEMON_URL, rpcUrl: DEFAULT_RPC_URL, log: [] };
  private socket: SocketLike | null = null;
  private status: SocketStatus = 'idle';
  private attempts = 0;
  private reconnectTimer: unknown = null;
  private closedByUs = false;
  private readonly openWaiters: Array<{ resolve: () => void; reject: (e: CoreError) => void; timer: unknown }> = [];
  private readonly expectations: Expectation[] = [];
  private readonly pending = new Map<string, Pending>();
  private readonly connectedOrigins = new Set<string>();
  private readonly stateListeners = new Set<(s: StateView) => void>();
  private readonly pageEventListeners = new Set<(name: PageEventName, ev: PageEvent) => void>();
  private lastError: string | null = null;

  private readonly now: () => number;
  private readonly setT: (fn: () => void, ms: number) => unknown;
  private readonly clearT: (h: unknown) => void;
  private readonly requestTimeoutMs: number;
  private readonly handshakeTimeoutMs: number;
  private readonly openTimeoutMs: number;
  private readonly maxBackoffMs: number;

  constructor(private readonly deps: CoreDeps) {
    this.now = deps.now ?? (() => Date.now());
    this.setT = deps.setTimeout ?? ((fn, ms) => globalThis.setTimeout(fn, ms));
    this.clearT = deps.clearTimeout ?? ((h) => globalThis.clearTimeout(h as number));
    this.requestTimeoutMs = deps.requestTimeoutMs ?? REQUEST_TIMEOUT_MS;
    this.handshakeTimeoutMs = deps.handshakeTimeoutMs ?? 15_000;
    this.openTimeoutMs = deps.openTimeoutMs ?? 10_000;
    this.maxBackoffMs = deps.maxBackoffMs ?? 30_000;
  }

  // ───────────── lifecycle ─────────────

  async init(): Promise<void> {
    const stored = await this.deps.storage.get();
    const evmChainId = findEvmChain(stored.evmChainId ?? NaN) ? stored.evmChainId : DEFAULT_EVM_CHAIN_ID;
    this.settings = {
      daemonUrl: stored.daemonUrl || DEFAULT_DAEMON_URL,
      rpcUrl: stored.rpcUrl || DEFAULT_RPC_URL,
      token: stored.token,
      pubkey: stored.pubkey,
      addresses: stored.addresses && typeof stored.addresses === 'object' ? stored.addresses : undefined,
      evmChainId,
      evmRpcUrls: stored.evmRpcUrls && typeof stored.evmRpcUrls === 'object' ? stored.evmRpcUrls : {},
      btcApiUrl: stored.btcApiUrl || DEFAULT_BTC_API_URL,
      takeoverEthereum: stored.takeoverEthereum === true,
      log: Array.isArray(stored.log) ? stored.log.slice(0, LOG_LIMIT) : [],
    };
    // Keep the dynamically registered takeover script in step with the stored toggle.
    void this.deps.applyTakeover?.(this.settings.takeoverEthereum === true).catch((e: Error) => {
      this.lastError = `window.ethereum takeover: ${e.message}`;
      this.emit();
    });
    this.openSocket();
    this.emit();
  }

  getState(): StateView {
    const p = this.pending.values().next().value as Pending | undefined;
    return {
      socket: this.status,
      daemonUrl: this.settings.daemonUrl,
      rpcUrl: this.settings.rpcUrl,
      paired: Boolean(this.settings.token),
      pubkey: this.settings.pubkey ?? null,
      addresses: this.addresses(),
      evmChainId: this.evmChainId(),
      evmRpcUrls: Object.fromEntries(EVM_CHAINS.map((c) => [String(c.id), this.evmRpcUrl(c.id)])),
      btcApiUrl: this.settings.btcApiUrl ?? DEFAULT_BTC_API_URL,
      takeoverEthereum: this.settings.takeoverEthereum === true,
      connectedOrigins: [...this.connectedOrigins],
      pending: p
        ? { id: p.id, kind: p.kind, chain: p.chain, origin: p.origin, startedAt: p.startedAt, explain: p.explain }
        : null,
      log: this.settings.log,
      lastError: this.lastError,
    };
  }

  onState(listener: (s: StateView) => void): () => void {
    this.stateListeners.add(listener);
    return () => this.stateListeners.delete(listener);
  }

  /** Unsolicited events for every connected page: `disconnected` (after `forget`), `evmChainChanged`. */
  onPageEvent(listener: (name: PageEventName, ev: PageEvent) => void): () => void {
    this.pageEventListeners.add(listener);
    return () => this.pageEventListeners.delete(listener);
  }

  private emit(): void {
    const s = this.getState();
    for (const l of this.stateListeners) l(s);
  }

  private emitPageEvent(ev: PageEvent): void {
    for (const l of this.pageEventListeners) l(ev.event, ev);
  }

  private addresses(): DaemonAddresses {
    const a = this.settings.addresses ?? {};
    return { ...a, solana: a.solana ?? this.settings.pubkey };
  }

  private evmChainId(): number {
    return this.settings.evmChainId ?? DEFAULT_EVM_CHAIN_ID;
  }

  private evmRpcUrl(chainId: number): string {
    const chain = findEvmChain(chainId);
    if (!chain) throw new CoreError('UNSUPPORTED_CHAIN', `chain ${chainId} is not supported`);
    return this.settings.evmRpcUrls?.[String(chainId)] || chain.defaultRpcUrl;
  }

  // ───────────── socket ─────────────

  private openSocket(): void {
    if (this.reconnectTimer) {
      this.clearT(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (!this.settings.daemonUrl) {
      this.status = 'idle';
      return;
    }
    this.closedByUs = false;
    this.status = this.attempts === 0 ? 'connecting' : 'reconnecting';
    let sock: SocketLike;
    try {
      sock = this.deps.createSocket(this.settings.daemonUrl);
    } catch (e) {
      this.lastError = `cannot open socket: ${(e as Error).message}`;
      this.scheduleReconnect();
      return;
    }
    this.socket = sock;
    sock.onopen = () => {
      if (this.socket !== sock) return;
      this.status = 'open';
      this.attempts = 0;
      this.lastError = null;
      for (const w of this.openWaiters.splice(0)) {
        this.clearT(w.timer);
        w.resolve();
      }
      this.emit();
    };
    sock.onmessage = (ev) => {
      if (this.socket !== sock) return;
      this.onData(ev.data);
    };
    sock.onerror = () => {
      if (this.socket !== sock) return;
      this.lastError = `socket error (${this.settings.daemonUrl})`;
    };
    sock.onclose = () => {
      if (this.socket !== sock) return;
      this.socket = null;
      this.failAll(new CoreError('ROBOT_OFFLINE', 'lost connection to the robot'));
      this.connectedOrigins.clear();
      if (this.closedByUs) {
        this.status = 'idle';
        this.emit();
        return;
      }
      this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    this.status = 'reconnecting';
    const delay = Math.min(this.maxBackoffMs, 1000 * 2 ** Math.min(this.attempts, 10));
    this.attempts++;
    this.reconnectTimer = this.setT(() => {
      this.reconnectTimer = null;
      this.openSocket();
    }, delay);
    this.emit();
  }

  private closeSocket(): void {
    this.closedByUs = true;
    if (this.reconnectTimer) {
      this.clearT(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const s = this.socket;
    this.socket = null;
    if (s) {
      try {
        s.close();
      } catch {
        /* ignore */
      }
    }
    this.failAll(new CoreError('ROBOT_OFFLINE', 'disconnected from the robot'));
    this.connectedOrigins.clear();
    this.status = 'idle';
  }

  private failAll(err: CoreError): void {
    for (const w of this.openWaiters.splice(0)) {
      this.clearT(w.timer);
      w.reject(err);
    }
    for (const e of this.expectations.splice(0)) {
      this.clearT(e.timer);
      e.reject(err);
    }
    for (const p of [...this.pending.values()]) this.settle(p, 'error', err);
  }

  private waitForOpen(): Promise<void> {
    if (this.status === 'open' && this.socket) return Promise.resolve();
    if (this.status === 'idle') {
      return Promise.reject(new CoreError('ROBOT_OFFLINE', 'no robot configured'));
    }
    return new Promise((resolve, reject) => {
      const waiter = {
        resolve,
        reject,
        timer: this.setT(() => {
          const i = this.openWaiters.indexOf(waiter);
          if (i >= 0) this.openWaiters.splice(i, 1);
          reject(new CoreError('ROBOT_OFFLINE', `robot not reachable at ${this.settings.daemonUrl}`));
        }, this.openTimeoutMs),
      };
      this.openWaiters.push(waiter);
    });
  }

  private send(msg: DaemonRequest): void {
    if (!this.socket || this.status !== 'open') {
      throw new CoreError('ROBOT_OFFLINE', 'robot is offline');
    }
    this.socket.send(JSON.stringify(msg) + '\n');
  }

  private onData(raw: unknown): void {
    if (typeof raw !== 'string') return;
    for (const line of raw.split('\n')) {
      const trimmed = line.trim();
      if (!trimmed) continue;
      let msg: DaemonMessage;
      try {
        msg = JSON.parse(trimmed) as DaemonMessage;
      } catch {
        this.lastError = 'daemon sent malformed JSON';
        continue;
      }
      this.onDaemonMessage(msg);
    }
  }

  private onDaemonMessage(msg: DaemonMessage): void {
    if (!msg || typeof msg !== 'object' || typeof msg.t !== 'string') return;
    // Handshake replies carry no id and are matched FIFO against what we sent.
    if (msg.t === 'paired' || msg.t === 'connected') {
      const exp = this.expectations.shift();
      if (!exp) return;
      this.clearT(exp.timer);
      if (exp.t === msg.t) exp.resolve(msg);
      else exp.reject(new CoreError('INTERNAL', `expected ${exp.t}, daemon sent ${msg.t}`));
      return;
    }
    if ((msg.t === 'denied' || msg.t === 'error') && !msg.id) {
      const exp = this.expectations.shift();
      if (!exp) return;
      this.clearT(exp.timer);
      exp.reject(new CoreError('DENIED', msg.reason || `${exp.t} refused`));
      return;
    }
    if (!('id' in msg) || !msg.id) return;
    const p = this.pending.get(msg.id);
    if (!p) return;
    switch (msg.t) {
      case 'explain':
        p.explain = {
          summary: msg.summary,
          risk: msg.risk,
          reasons: Array.isArray(msg.reasons) ? msg.reasons : [],
          lines: Array.isArray(msg.lines) && msg.lines.length ? msg.lines : undefined,
        };
        this.emit();
        return;
      case 'signed':
        this.settle(p, 'signed', msg);
        return;
      case 'denied':
      case 'error':
        this.settle(p, 'denied', new CoreError('DENIED', msg.reason || 'denied'));
        return;
    }
  }

  private expect(t: Expectation['t']): Promise<DaemonMessage> {
    return new Promise((resolve, reject) => {
      const exp: Expectation = {
        t,
        resolve,
        reject,
        timer: this.setT(() => {
          const i = this.expectations.indexOf(exp);
          if (i >= 0) this.expectations.splice(i, 1);
          reject(new CoreError('TIMEOUT', `${t} timed out`));
        }, this.handshakeTimeoutMs),
      };
      this.expectations.push(exp);
    });
  }

  // ───────────── popup commands ─────────────

  async setDaemonUrl(url: string): Promise<void> {
    const u = url.trim();
    if (!/^wss?:\/\/[^\s/]+(:\d+)?\/?$/.test(u)) {
      throw new CoreError('BAD_REQUEST', 'robot address must look like ws://host:7401');
    }
    this.closeSocket();
    this.settings.daemonUrl = u.replace(/\/$/, '');
    await this.deps.storage.set({ daemonUrl: this.settings.daemonUrl });
    this.attempts = 0;
    this.openSocket();
    this.emit();
  }

  async setRpcUrl(url: string): Promise<void> {
    const u = url.trim() || DEFAULT_RPC_URL;
    if (!/^https?:\/\//.test(u)) throw new CoreError('BAD_REQUEST', 'RPC URL must be http(s)');
    this.settings.rpcUrl = u;
    await this.deps.storage.set({ rpcUrl: u });
    this.emit();
  }

  /** Popup-driven chain switch; also what `wallet_switchEthereumChain` ends up calling. */
  async setEvmChain(chainId: number): Promise<void> {
    if (!findEvmChain(chainId)) throw new CoreError('UNSUPPORTED_CHAIN', `chain ${chainId} is not supported`);
    if (this.settings.evmChainId === chainId) return;
    this.settings.evmChainId = chainId;
    await this.deps.storage.set({ evmChainId: chainId });
    this.emitPageEvent({ event: 'evmChainChanged', chainId: chainIdHex(chainId) });
    this.emit();
  }

  /** Empty URL restores the chain's default. */
  async setEvmRpcUrl(chainId: number, url: string): Promise<void> {
    if (!findEvmChain(chainId)) throw new CoreError('UNSUPPORTED_CHAIN', `chain ${chainId} is not supported`);
    const u = url.trim();
    if (u && !/^https?:\/\//.test(u)) throw new CoreError('BAD_REQUEST', 'RPC URL must be http(s)');
    const next = { ...(this.settings.evmRpcUrls ?? {}) };
    if (u) next[String(chainId)] = u;
    else delete next[String(chainId)];
    this.settings.evmRpcUrls = next;
    await this.deps.storage.set({ evmRpcUrls: next });
    this.emit();
  }

  async setBtcApiUrl(url: string): Promise<void> {
    const u = url.trim() || DEFAULT_BTC_API_URL;
    if (!/^https?:\/\//.test(u)) throw new CoreError('BAD_REQUEST', 'bitcoin API URL must be http(s)');
    this.settings.btcApiUrl = u.replace(/\/$/, '');
    await this.deps.storage.set({ btcApiUrl: this.settings.btcApiUrl });
    this.emit();
  }

  async setTakeoverEthereum(on: boolean): Promise<void> {
    await this.deps.applyTakeover?.(on);
    this.settings.takeoverEthereum = on;
    await this.deps.storage.set({ takeoverEthereum: on });
    this.emit();
  }

  reconnect(): void {
    this.closeSocket();
    this.attempts = 0;
    this.openSocket();
    this.emit();
  }

  async pair(code: string): Promise<{ pubkey: string }> {
    const c = code.replace(/\D/g, '');
    if (c.length !== 6) throw new CoreError('BAD_REQUEST', 'pairing code is 6 digits');
    await this.waitForOpen();
    const reply = this.expect('paired');
    this.send({ t: 'pair', code: c });
    const m = (await reply) as DaemonMessage & { t: 'paired' };
    if (!m.token || !m.pubkey) throw new CoreError('INTERNAL', 'daemon returned an incomplete pairing');
    this.settings.token = m.token;
    this.settings.pubkey = m.pubkey;
    this.connectedOrigins.clear();
    const patch: Partial<Settings> = { token: m.token, pubkey: m.pubkey };
    if (m.addresses && typeof m.addresses === 'object') {
      this.settings.addresses = { ...m.addresses, solana: m.addresses.solana ?? m.pubkey };
      patch.addresses = this.settings.addresses;
    }
    await this.deps.storage.set(patch);
    this.emit();
    return { pubkey: m.pubkey };
  }

  async forget(): Promise<void> {
    this.settings.token = undefined;
    this.settings.pubkey = undefined;
    this.settings.addresses = undefined;
    this.connectedOrigins.clear();
    await this.deps.storage.set({ token: undefined, pubkey: undefined, addresses: undefined });
    for (const p of [...this.pending.values()]) {
      this.settle(p, 'error', new CoreError('NOT_PAIRED', 'robot was unpaired'));
    }
    this.emitPageEvent({ event: 'disconnected' });
    this.emit();
  }

  // ───────────── page requests ─────────────

  async handlePageRequest<M extends PageMethod>(
    origin: string,
    req: PageRequest<M>,
  ): Promise<PageResults[M]> {
    type R = PageResults[M];
    switch (req.method) {
      case 'connect':
        return (await this.connectOrigin(origin)) as R;
      case 'disconnect':
        this.connectedOrigins.delete(origin);
        this.emit();
        return {} as R;
      case 'signTransaction': {
        const p = req.params as PageParams['signTransaction'];
        if (typeof p.tx !== 'string' || !p.tx) throw new CoreError('BAD_REQUEST', 'missing tx');
        const id = newId('t');
        const signed = await this.roundTrip(origin, 'signTransaction', id, {
          t: 'signTransaction',
          id,
          chain: 'solana',
          tx: p.tx,
          origin,
        });
        if (!signed.tx) throw new CoreError('INTERNAL', 'daemon returned no tx');
        return { tx: signed.tx } as R;
      }
      case 'signMessage': {
        const p = req.params as PageParams['signMessage'];
        if (typeof p.message !== 'string') throw new CoreError('BAD_REQUEST', 'missing message');
        const id = newId('m');
        const signed = await this.roundTrip(origin, 'signMessage', id, {
          t: 'signMessage',
          id,
          chain: 'solana',
          message: p.message,
          origin,
        });
        if (!signed.signature) throw new CoreError('INTERNAL', 'daemon returned no signature');
        return { signature: signed.signature } as R;
      }
      case 'signAndSendTransaction': {
        const p = req.params as PageParams['signAndSendTransaction'];
        if (typeof p.tx !== 'string' || !p.tx) throw new CoreError('BAD_REQUEST', 'missing tx');
        const rpc = this.rpcFor(p.chain);
        const id = newId('t');
        const signed = await this.roundTrip(origin, 'signAndSendTransaction', id, {
          t: 'signTransaction',
          id,
          chain: 'solana',
          tx: p.tx,
          origin,
        });
        if (!signed.tx) throw new CoreError('INTERNAL', 'daemon returned no tx');
        try {
          const sig = await this.deps.sendRawTransaction(rpc, fromBase64(signed.tx), p.options);
          this.amendLog(id, { detail: `sent ${sig.slice(0, 8)}…` });
          return { signature: sig } as R;
        } catch (e) {
          const msg = (e as Error).message ?? String(e);
          this.amendLog(id, { outcome: 'error', detail: `send failed: ${msg}` });
          throw new CoreError('SEND_FAILED', msg);
        }
      }

      // ── shared ──
      case 'connectChain': {
        const p = req.params as PageParams['connectChain'];
        if (p.chain !== 'evm' && p.chain !== 'bitcoin') throw new CoreError('BAD_REQUEST', `unknown chain ${String(p.chain)}`);
        const chainId = chainIdHex(this.evmChainId());
        const already = this.connectedOrigins.has(origin) && this.status === 'open';
        if (p.silent && !already) return { address: null, chainId } as R;
        if (!already) await this.connectOrigin(origin);
        const a = this.addresses();
        const address = p.chain === 'evm' ? a.evm : a.bitcoin;
        if (!address) {
          if (p.silent) return { address: null, chainId } as R;
          throw new CoreError('NOT_CONNECTED', `the robot exposes no ${p.chain} address`);
        }
        const out: PageResults['connectChain'] = { address, chainId };
        if (p.chain === 'bitcoin' && a.bitcoinPublicKey) out.publicKey = a.bitcoinPublicKey;
        return out as R;
      }

      // ── evm ──
      case 'evm_chain':
        return { chainId: chainIdHex(this.evmChainId()) } as R;
      case 'evm_switchChain': {
        const p = req.params as PageParams['evm_switchChain'];
        const id = parseChainId(p.chainId);
        if (!Number.isFinite(id)) throw new CoreError('BAD_REQUEST', 'chainId must be a hex string');
        if (!findEvmChain(id)) throw new CoreError('UNSUPPORTED_CHAIN', `chain ${id} is not supported by Pincer1`);
        await this.setEvmChain(id);
        return { chainId: chainIdHex(id) } as R;
      }
      case 'evm_rpc': {
        const p = req.params as PageParams['evm_rpc'];
        if (typeof p.method !== 'string' || !EVM_READONLY_METHODS.has(p.method)) {
          throw new CoreError('UNSUPPORTED_METHOD', `${String(p.method)} is not proxied`);
        }
        const result = await this.evmRpc(this.evmChainId(), p.method, Array.isArray(p.params) ? p.params : []);
        return { result } as R;
      }
      case 'evm_sendTransaction':
      case 'evm_signTransaction': {
        const p = req.params as PageParams['evm_sendTransaction'];
        const send = req.method === 'evm_sendTransaction';
        const chainId = this.evmChainId();
        // The handshake must have happened so we know the EVM address `from` has to match.
        if (!this.connectedOrigins.has(origin) || this.status !== 'open') await this.connectOrigin(origin);
        const account = this.addresses().evm;
        if (!account) throw new CoreError('NOT_CONNECTED', 'the robot exposes no evm address');
        const rpc = (method: string, params: unknown[]): Promise<unknown> => this.evmRpc(chainId, method, params);
        let prepared;
        try {
          prepared = await prepareEvmTransaction(p.tx, chainId, account, rpc);
        } catch (e) {
          if (e instanceof EvmTxError) throw new CoreError(e.code, e.message);
          throw e;
        }
        const id = newId('e');
        const signed = await this.roundTrip(origin, send ? 'evm_sendTransaction' : 'evm_signTransaction', id, {
          t: 'signTransaction',
          id,
          chain: 'evm',
          tx: prepared.unsigned,
          chainId,
          origin,
        });
        let raw: string;
        try {
          raw = checkSignedEvmTransaction(signed.tx, prepared);
        } catch (e) {
          this.amendLog(id, { outcome: 'error', detail: (e as Error).message });
          throw new CoreError('INTERNAL', (e as Error).message);
        }
        if (!send) return { tx: raw } as R;
        try {
          const hash = await rpc('eth_sendRawTransaction', [raw]);
          if (typeof hash !== 'string') throw new Error('node returned no hash');
          this.amendLog(id, { detail: `sent ${hash.slice(0, 10)}…` });
          return { hash } as R;
        } catch (e) {
          const msg = (e as Error).message ?? String(e);
          this.amendLog(id, { outcome: 'error', detail: `send failed: ${msg}` });
          throw new CoreError('SEND_FAILED', msg);
        }
      }
      case 'evm_signMessage': {
        const p = req.params as PageParams['evm_signMessage'];
        if (typeof p.message !== 'string' || !/^0x([0-9a-fA-F]{2})*$/.test(p.message)) {
          throw new CoreError('BAD_REQUEST', 'message must be 0x hex bytes');
        }
        const id = newId('e');
        const signed = await this.roundTrip(origin, 'evm_signMessage', id, {
          t: 'signMessage',
          id,
          chain: 'evm',
          message: p.message,
          origin,
        });
        return { signature: this.hexSignature(signed) } as R;
      }
      case 'evm_signTypedData': {
        const p = req.params as PageParams['evm_signTypedData'];
        const td = p.typedData as Partial<TypedData> | undefined;
        if (!td || typeof td !== 'object' || !td.types || typeof td.primaryType !== 'string' || !td.domain || !td.message) {
          throw new CoreError('BAD_REQUEST', 'typedData needs types, primaryType, domain and message');
        }
        const domainChain = (td.domain as { chainId?: unknown }).chainId;
        if (domainChain !== undefined) {
          const want = parseChainId(typeof domainChain === 'bigint' ? Number(domainChain) : domainChain);
          if (Number.isFinite(want) && want !== this.evmChainId()) {
            throw new CoreError('BAD_REQUEST', `typed data is for chain ${want}, wallet is on ${this.evmChainId()}`);
          }
        }
        const id = newId('e');
        const signed = await this.roundTrip(origin, 'evm_signTypedData', id, {
          t: 'signMessage',
          id,
          chain: 'evm',
          typedData: { types: td.types, primaryType: td.primaryType, domain: td.domain, message: td.message },
          origin,
        });
        return { signature: this.hexSignature(signed) } as R;
      }

      // ── bitcoin ──
      case 'btc_signPsbt': {
        const p = req.params as PageParams['btc_signPsbt'];
        if (typeof p.psbt !== 'string' || !/^[A-Za-z0-9+/]+=*$/.test(p.psbt)) throw new CoreError('BAD_REQUEST', 'psbt must be base64');
        const broadcast = p.broadcast === true;
        const id = newId('b');
        const signed = await this.roundTrip(origin, 'btc_signPsbt', id, {
          t: 'signTransaction',
          id,
          chain: 'bitcoin',
          psbt: p.psbt,
          signInputs: p.signInputs,
          finalize: broadcast,
          origin,
        });
        if (!signed.psbt) throw new CoreError('INTERNAL', 'daemon returned no psbt');
        if (!broadcast) return { psbt: signed.psbt } as R;
        if (!signed.tx || !/^([0-9a-fA-F]{2})+$/.test(signed.tx)) {
          this.amendLog(id, { outcome: 'error', detail: 'daemon did not finalize the PSBT' });
          throw new CoreError('SEND_FAILED', 'the robot signed but did not finalize the PSBT, so it cannot be broadcast');
        }
        if (!this.deps.broadcastBitcoin) throw new CoreError('SEND_FAILED', 'bitcoin broadcast is not available');
        try {
          const txid = await this.deps.broadcastBitcoin(this.settings.btcApiUrl ?? DEFAULT_BTC_API_URL, signed.tx);
          this.amendLog(id, { detail: `sent ${txid.slice(0, 10)}…` });
          return { psbt: signed.psbt, txid } as R;
        } catch (e) {
          const msg = (e as Error).message ?? String(e);
          this.amendLog(id, { outcome: 'error', detail: `send failed: ${msg}` });
          throw new CoreError('SEND_FAILED', msg);
        }
      }
      case 'btc_signMessage': {
        const p = req.params as PageParams['btc_signMessage'];
        if (typeof p.message !== 'string') throw new CoreError('BAD_REQUEST', 'missing message');
        if (typeof p.address !== 'string' || !p.address) throw new CoreError('BAD_REQUEST', 'missing address');
        if (!this.connectedOrigins.has(origin) || this.status !== 'open') await this.connectOrigin(origin);
        const mine = this.addresses().bitcoin;
        if (!mine || mine !== p.address) throw new CoreError('NOT_CONNECTED', 'address is not the Pincer1 bitcoin account');
        const id = newId('b');
        const signed = await this.roundTrip(origin, 'btc_signMessage', id, {
          t: 'signMessage',
          id,
          chain: 'bitcoin',
          message: p.message,
          address: p.address,
          origin,
        });
        if (!signed.signature) throw new CoreError('INTERNAL', 'daemon returned no signature');
        return { signature: signed.signature } as R;
      }
      default:
        throw new CoreError('BAD_REQUEST', `unknown method ${String(req.method)}`);
    }
  }

  private hexSignature(signed: DaemonMessage & { t: 'signed' }): string {
    const s = signed.signature;
    if (typeof s !== 'string' || !/^0x[0-9a-fA-F]+$/.test(s)) throw new CoreError('INTERNAL', 'daemon returned no 0x signature');
    return s;
  }

  private async evmRpc(chainId: number, method: string, params: unknown[]): Promise<unknown> {
    if (!this.deps.jsonRpc) throw new CoreError('INTERNAL', 'no EVM RPC client configured');
    const url = this.evmRpcUrl(chainId);
    try {
      return await this.deps.jsonRpc(url, method, params);
    } catch (e) {
      if (e instanceof RpcError) throw new CoreError('RPC_ERROR', e.message, { code: e.code, data: e.data });
      throw new CoreError('RPC_ERROR', (e as Error).message ?? String(e), { code: -32603 });
    }
  }

  private rpcFor(chain: string | undefined): string {
    const c = chain ?? 'solana:mainnet';
    if (!(c in RPC_BY_CHAIN)) throw new CoreError('BAD_REQUEST', `unsupported chain ${c}`);
    return RPC_BY_CHAIN[c] ?? this.settings.rpcUrl;
  }

  private async connectOrigin(origin: string): Promise<{ pubkey: string }> {
    if (!this.settings.token) {
      throw new CoreError('NOT_PAIRED', 'Pincer1 is not paired with a robot — open the extension popup');
    }
    if (this.connectedOrigins.has(origin) && this.status === 'open' && this.settings.pubkey) {
      return { pubkey: this.settings.pubkey };
    }
    await this.waitForOpen();
    const reply = this.expect('connected');
    this.send({ t: 'connect', token: this.settings.token, origin });
    const m = (await reply) as DaemonMessage & { t: 'connected' };
    const pubkey = m.pubkey || m.addresses?.solana;
    if (!pubkey) throw new CoreError('INTERNAL', 'daemon returned no pubkey');
    const patch: Partial<Settings> = {};
    if (pubkey !== this.settings.pubkey) {
      this.settings.pubkey = pubkey;
      patch.pubkey = pubkey;
    }
    if (m.addresses && typeof m.addresses === 'object') {
      const next: DaemonAddresses = { ...m.addresses, solana: m.addresses.solana ?? pubkey };
      if (JSON.stringify(next) !== JSON.stringify(this.settings.addresses ?? {})) {
        this.settings.addresses = next;
        patch.addresses = next;
      }
    }
    if (Object.keys(patch).length) await this.deps.storage.set(patch);
    this.connectedOrigins.add(origin);
    this.emit();
    return { pubkey };
  }

  private async roundTrip(
    origin: string,
    kind: RequestKind,
    id: string,
    msg: DaemonRequest,
  ): Promise<DaemonMessage & { t: 'signed' }> {
    // A page that is still "connected" after a worker restart re-runs the handshake transparently.
    if (!this.connectedOrigins.has(origin) || this.status !== 'open') await this.connectOrigin(origin);
    const result = new Promise<DaemonMessage & { t: 'signed' }>((resolve, reject) => {
      const p: Pending = {
        id,
        kind,
        chain: KIND_CHAIN[kind],
        origin,
        startedAt: this.now(),
        explain: null,
        resolve,
        reject,
        timer: null,
      };
      p.timer = this.setT(() => {
        this.settle(p, 'timeout', new CoreError('TIMEOUT', 'the robot did not answer in time'));
      }, this.requestTimeoutMs);
      this.pending.set(id, p);
    });
    this.updateBadge();
    this.emit();
    try {
      this.send(msg);
    } catch (e) {
      const p = this.pending.get(id);
      if (p) this.settle(p, 'error', e as CoreError);
    }
    return result;
  }

  private settle(
    p: Pending,
    outcome: 'signed' | 'denied' | 'timeout' | 'error',
    payload: (DaemonMessage & { t: 'signed' }) | CoreError,
  ): void {
    if (!this.pending.has(p.id)) return;
    this.pending.delete(p.id);
    this.clearT(p.timer);
    const entry: LogEntry = {
      id: p.id,
      kind: p.kind,
      chain: p.chain,
      origin: p.origin,
      ts: this.now(),
      outcome,
      summary: p.explain?.summary,
      risk: p.explain?.risk,
      detail: payload instanceof CoreError ? payload.message : undefined,
    };
    this.pushLog(entry);
    this.updateBadge();
    if (payload instanceof CoreError) p.reject(payload);
    else p.resolve(payload);
    this.emit();
  }

  private pushLog(entry: LogEntry): void {
    this.settings.log = [entry, ...this.settings.log].slice(0, LOG_LIMIT);
    void this.deps.storage.set({ log: this.settings.log });
  }

  private amendLog(id: string, patch: Partial<LogEntry>): void {
    this.settings.log = this.settings.log.map((e) => (e.id === id ? { ...e, ...patch } : e));
    void this.deps.storage.set({ log: this.settings.log });
    this.emit();
  }

  private updateBadge(): void {
    const n = this.pending.size;
    this.deps.setBadge(n ? String(n) : '');
  }
}
