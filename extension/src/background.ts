/**
 * MV3 service worker. Wires BackgroundCore to chrome.* APIs: storage, ports, badge, the
 * @solana/web3.js send used by signAndSendTransaction, fetch-based EVM JSON-RPC and bitcoin
 * broadcast, and the dynamically registered `window.ethereum` takeover script.
 */
import { Connection } from '@solana/web3.js';
import { BackgroundCore, CoreError, type SendOptions, type SocketLike, type StorageLike } from './background/core';
import { broadcastBitcoinTx, jsonRpc } from './lib/jsonRpc';
import {
  PORT_PAGE,
  PORT_POPUP,
  isPageRequest,
  type PageReply,
  type PopupCommand,
  type PopupMessage,
  type Settings,
  type WalletError,
} from './protocol';
import { findEvmChain } from './evm/chains';

const STORAGE_KEYS: (keyof Settings)[] = [
  'daemonUrl',
  'rpcUrl',
  'token',
  'pubkey',
  'addresses',
  'evmChainId',
  'evmRpcUrls',
  'btcApiUrl',
  'takeoverEthereum',
  'log',
];

const storage: StorageLike = {
  async get() {
    return (await chrome.storage.local.get(STORAGE_KEYS)) as Partial<Settings>;
  },
  async set(patch) {
    const toSet: Record<string, unknown> = {};
    const toRemove: string[] = [];
    for (const [k, v] of Object.entries(patch)) {
      if (v === undefined) toRemove.push(k);
      else toSet[k] = v;
    }
    if (Object.keys(toSet).length) await chrome.storage.local.set(toSet);
    if (toRemove.length) await chrome.storage.local.remove(toRemove);
  },
};

function createSocket(url: string): SocketLike {
  const ws = new WebSocket(url);
  const like: SocketLike = {
    send: (d) => ws.send(d),
    close: () => ws.close(),
    onopen: null,
    onmessage: null,
    onclose: null,
    onerror: null,
  };
  ws.onopen = () => like.onopen?.();
  ws.onmessage = (ev) => like.onmessage?.({ data: ev.data });
  ws.onclose = () => like.onclose?.();
  ws.onerror = () => like.onerror?.();
  return like;
}

async function sendRawTransaction(rpcUrl: string, signedTx: Uint8Array, options: SendOptions): Promise<string> {
  const connection = new Connection(rpcUrl, options?.preflightCommitment ?? 'confirmed');
  return connection.sendRawTransaction(signedTx, {
    skipPreflight: options?.skipPreflight,
    preflightCommitment: options?.preflightCommitment,
    maxRetries: options?.maxRetries,
    minContextSlot: options?.minContextSlot,
  });
}

function setBadge(text: string): void {
  void chrome.action.setBadgeText({ text });
  if (text) {
    void chrome.action.setBadgeBackgroundColor({ color: '#8ad414' });
    void chrome.action.setBadgeTextColor?.({ color: '#0a0a0a' });
  }
}

/**
 * The "take over window.ethereum" toggle: register / unregister takeover.js as a MAIN-world
 * document_start script. Doing it this way (instead of asking storage from the page) makes the
 * decision deterministic — both scripts run before any page script, no race.
 */
const TAKEOVER_SCRIPT_ID = 'pincer1-takeover-ethereum';
async function applyTakeover(on: boolean): Promise<void> {
  const existing = await chrome.scripting.getRegisteredContentScripts({ ids: [TAKEOVER_SCRIPT_ID] });
  if (on && existing.length === 0) {
    await chrome.scripting.registerContentScripts([
      {
        id: TAKEOVER_SCRIPT_ID,
        js: ['takeover.js'],
        matches: ['<all_urls>'],
        runAt: 'document_start',
        world: 'MAIN',
        allFrames: true,
        persistAcrossSessions: true,
      },
    ]);
  } else if (!on && existing.length > 0) {
    await chrome.scripting.unregisterContentScripts({ ids: [TAKEOVER_SCRIPT_ID] });
  }
}

const core = new BackgroundCore({
  createSocket,
  storage,
  setBadge,
  sendRawTransaction,
  jsonRpc: (url, method, params) => jsonRpc(url, method, params),
  broadcastBitcoin: (api, hex) => broadcastBitcoinTx(api, hex),
  applyTakeover,
});

// ── MV3 keep-alive: Chrome kills an idle worker after 30 s, which would drop the WebSocket while
// the robot waits for a touch. Any extension API call resets the idle timer.
let keepAlive: ReturnType<typeof setInterval> | null = null;
core.onState((s) => {
  const wanted = s.socket !== 'idle';
  if (wanted && !keepAlive) {
    keepAlive = setInterval(() => void chrome.runtime.getPlatformInfo(), 20_000);
  } else if (!wanted && keepAlive) {
    clearInterval(keepAlive);
    keepAlive = null;
  }
});

const pagePorts = new Set<chrome.runtime.Port>();
const popupPorts = new Set<chrome.runtime.Port>();

function portOrigin(port: chrome.runtime.Port): string | null {
  const s = port.sender;
  if (!s) return null;
  if (s.origin) return s.origin;
  if (s.url) {
    try {
      return new URL(s.url).origin;
    } catch {
      return null;
    }
  }
  return null;
}

function toError(e: unknown): WalletError {
  if (e instanceof CoreError) return e.toJSON();
  return { code: 'INTERNAL', message: (e as Error)?.message ?? String(e) };
}

chrome.runtime.onConnect.addListener((port) => {
  if (port.name === PORT_PAGE) {
    const origin = portOrigin(port);
    if (!origin || origin === 'null') {
      port.disconnect();
      return;
    }
    pagePorts.add(port);
    port.onDisconnect.addListener(() => pagePorts.delete(port));
    port.onMessage.addListener((msg: unknown) => {
      if (!isPageRequest(msg)) return;
      core.handlePageRequest(origin, msg).then(
        (result) => {
          const reply: PageReply = { channel: 'pincer1', dir: 'ext→page', id: msg.id, ok: true, result };
          try {
            port.postMessage(reply);
          } catch {
            /* page went away */
          }
        },
        (err) => {
          const reply: PageReply = { channel: 'pincer1', dir: 'ext→page', id: msg.id, ok: false, error: toError(err) };
          try {
            port.postMessage(reply);
          } catch {
            /* page went away */
          }
        },
      );
    });
    return;
  }

  if (port.name === PORT_POPUP) {
    popupPorts.add(port);
    port.onDisconnect.addListener(() => popupPorts.delete(port));
    const push = (m: PopupMessage): void => {
      try {
        port.postMessage(m);
      } catch {
        /* popup closed */
      }
    };
    push({ t: 'state', state: core.getState() });
    port.onMessage.addListener((cmd: PopupCommand) => {
      void runPopupCommand(cmd).then(
        (text) => {
          if (text) push({ t: 'toast', text, kind: 'ok' });
        },
        (err) => push({ t: 'toast', text: toError(err).message, kind: 'err' }),
      );
    });
  }
});

async function runPopupCommand(cmd: PopupCommand): Promise<string | null> {
  switch (cmd.t) {
    case 'setDaemonUrl':
      await core.setDaemonUrl(cmd.url);
      return 'robot address saved';
    case 'setRpcUrl':
      await core.setRpcUrl(cmd.url);
      return 'Solana RPC saved';
    case 'setEvmChain':
      await core.setEvmChain(cmd.chainId);
      return `switched to ${findEvmChain(cmd.chainId)?.name ?? cmd.chainId}`;
    case 'setEvmRpcUrl':
      await core.setEvmRpcUrl(cmd.chainId, cmd.url);
      return cmd.url.trim() ? `${findEvmChain(cmd.chainId)?.name ?? cmd.chainId} RPC saved` : 'RPC reset to default';
    case 'setBtcApiUrl':
      await core.setBtcApiUrl(cmd.url);
      return 'bitcoin API saved';
    case 'setTakeoverEthereum':
      await core.setTakeoverEthereum(cmd.on);
      return cmd.on ? 'window.ethereum takeover on — reload open tabs' : 'window.ethereum takeover off — reload open tabs';
    case 'pair': {
      const { pubkey } = await core.pair(cmd.code);
      return `paired · ${pubkey.slice(0, 4)}…${pubkey.slice(-4)}`;
    }
    case 'forget':
      await core.forget();
      return 'unpaired';
    case 'reconnect':
      core.reconnect();
      return null;
    default:
      return null;
  }
}

core.onState((state) => {
  for (const p of popupPorts) {
    try {
      p.postMessage({ t: 'state', state } satisfies PopupMessage);
    } catch {
      /* ignore */
    }
  }
});

core.onPageEvent((_name, event) => {
  const reply: PageReply = { channel: 'pincer1', dir: 'ext→page', ...event };
  for (const p of pagePorts) {
    try {
      p.postMessage(reply);
    } catch {
      /* ignore */
    }
  }
});

const ready = core.init();
chrome.runtime.onInstalled.addListener(() => void ready);
chrome.runtime.onStartup.addListener(() => void ready);
