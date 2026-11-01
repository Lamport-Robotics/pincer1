import {
  DEFAULT_BTC_API_URL,
  DEFAULT_DAEMON_URL,
  DEFAULT_RPC_URL,
  PORT_POPUP,
  type Chain,
  type LogEntry,
  type PopupCommand,
  type PopupMessage,
  type RequestKind,
  type StateView,
} from '../protocol';
import { EVM_CHAINS, chainIdHex } from '../evm/chains';

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} missing`);
  return el as T;
};

const ui = {
  status: $('status'),
  statusText: $('statusText'),
  pendingCard: $('pendingCard'),
  pendingChain: $('pendingChain'),
  pendingKind: $('pendingKind'),
  pendingRisk: $('pendingRisk'),
  pendingSummary: $('pendingSummary'),
  pendingReasons: $('pendingReasons'),
  pendingLines: $('pendingLines'),
  pendingTimer: $('pendingTimer'),
  pendingOrigin: $('pendingOrigin'),
  hostForm: $<HTMLFormElement>('hostForm'),
  host: $<HTMLInputElement>('host'),
  reconnect: $<HTMLButtonElement>('reconnect'),
  pairBlock: $('pairBlock'),
  pairForm: $<HTMLFormElement>('pairForm'),
  code: $<HTMLInputElement>('code'),
  pairedBlock: $('pairedBlock'),
  addrSolana: $<HTMLButtonElement>('addrSolana'),
  addrEvm: $<HTMLButtonElement>('addrEvm'),
  addrBitcoin: $<HTMLButtonElement>('addrBitcoin'),
  addrHint: $('addrHint'),
  origins: $('origins'),
  sites: $('sites'),
  approvalCard: $('approvalCard'),
  approvals: $('approvals'),
  forget: $<HTMLButtonElement>('forget'),
  evmChain: $<HTMLSelectElement>('evmChain'),
  evmChainId: $('evmChainId'),
  evmRpcForm: $<HTMLFormElement>('evmRpcForm'),
  evmRpc: $<HTMLInputElement>('evmRpc'),
  takeover: $<HTMLInputElement>('takeover'),
  rpcForm: $<HTMLFormElement>('rpcForm'),
  rpc: $<HTMLInputElement>('rpc'),
  btcForm: $<HTMLFormElement>('btcForm'),
  btcApi: $<HTMLInputElement>('btcApi'),
  log: $('log'),
  logEmpty: $('logEmpty'),
  logCount: $('logCount'),
  toast: $('toast'),
  idleCard: $('idleCard'),
  idleHint: $('idleHint'),
  sitesEmpty: $('sitesEmpty'),
  pendingSiteMark: $('pendingSiteMark'),
};

const DEMO = typeof chrome === 'undefined' || !chrome.runtime?.connect || new URLSearchParams(location.search).has('demo');
let port: chrome.runtime.Port = DEMO
  ? ({ postMessage() {}, onMessage: { addListener() {} }, onDisconnect: { addListener() {} } } as unknown as chrome.runtime.Port)
  : chrome.runtime.connect({ name: PORT_POPUP });
let state: StateView | null = null;
let hostDirty = false;
let rpcDirty = false;
let evmRpcDirty = false;
let btcDirty = false;
/** Chain whose RPC the field is showing; follows the wallet's chain until the user picks one. */
let shownChainId: number | null = null;
let toastTimer: ReturnType<typeof setTimeout> | null = null;

function send(cmd: PopupCommand): void {
  try {
    port.postMessage(cmd);
  } catch {
    port = chrome.runtime.connect({ name: PORT_POPUP });
    attach(port);
    port.postMessage(cmd);
  }
}

function attach(p: chrome.runtime.Port): void {
  p.onMessage.addListener((m: PopupMessage) => {
    if (m.t === 'state') {
      state = m.state;
      render();
    } else if (m.t === 'toast') {
      toast(m.text, m.kind);
    }
  });
  p.onDisconnect.addListener(() => {
    // worker restarted; reconnect for a fresh snapshot
    setTimeout(() => {
      port = chrome.runtime.connect({ name: PORT_POPUP });
      attach(port);
    }, 300);
  });
}
attach(port);

function toast(text: string, kind: 'ok' | 'err'): void {
  ui.toast.textContent = text;
  ui.toast.dataset.kind = kind;
  ui.toast.hidden = false;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (ui.toast.hidden = true), 2600);
}

const STATUS_LABEL: Record<StateView['socket'], string> = {
  idle: 'Not connected',
  connecting: 'Connecting',
  open: 'Robot online',
  reconnecting: 'Reconnecting',
};

const CHAIN_LABEL: Record<Chain, string> = { solana: 'Solana', evm: 'Ethereum', bitcoin: 'Bitcoin' };
const RISK_LABEL: Record<string, string> = {
  none: 'Reading',
  low: 'Looks fine',
  medium: 'Worth a look',
  high: 'High risk',
  block: 'Blocked',
};

/** Human label for what the dapp asked, per chain. */
const KIND_LABEL: Record<RequestKind, string> = {
  signTransaction: 'sign transaction',
  signMessage: 'sign message',
  signAndSendTransaction: 'sign & send',
  evm_sendTransaction: 'send transaction',
  evm_signTransaction: 'sign transaction',
  evm_signMessage: 'personal_sign',
  evm_signTypedData: 'typed data (EIP-712)',
  btc_signPsbt: 'sign PSBT',
  btc_signMessage: 'sign message (BIP-322)',
};

function chainOf(e: { chain?: Chain; kind: RequestKind }): Chain {
  if (e.chain) return e.chain;
  return e.kind.startsWith('evm_') ? 'evm' : e.kind.startsWith('btc_') ? 'bitcoin' : 'solana';
}

function shortKey(k: string): string {
  return k.length > 12 ? `${k.slice(0, 4)}…${k.slice(-4)}` : k;
}

function ago(ts: number): string {
  const s = Math.max(0, Math.round((Date.now() - ts) / 1000));
  if (s < 60) return `${s}s`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m}m`;
  const h = Math.round(m / 60);
  return h < 48 ? `${h}h` : `${Math.round(h / 24)}d`;
}

function originLabel(origin: string): string {
  try {
    return new URL(origin).host;
  } catch {
    return origin;
  }
}

function list(ul: HTMLElement, items: string[] | undefined): void {
  ul.replaceChildren(
    ...(items ?? []).map((r) => {
      const li = document.createElement('li');
      li.textContent = r;
      return li;
    }),
  );
}

/** "fee 0.000005 SOL" -> a label and a value; a line without that shape stays one cell. */
function rows(ul: HTMLElement, items: string[] | undefined): void {
  ul.replaceChildren(
    ...(items ?? []).map((line) => {
      const li = document.createElement('li');
      const m = /^([A-Za-z][\w ]{0,14}?)\s+(.+)$/.exec(line.trim());
      if (m && m[2].length > 1) {
        const k = document.createElement('span');
        k.className = 'k';
        k.textContent = m[1];
        const v = document.createElement('span');
        v.className = 'vv';
        v.textContent = m[2];
        v.title = m[2];
        li.append(k, v);
      } else {
        const v = document.createElement('span');
        v.className = 'vv';
        v.style.textAlign = 'left';
        v.textContent = line;
        li.append(v);
      }
      return li;
    }),
  );
}

function renderAddress(btn: HTMLButtonElement, value: string | undefined): void {
  const addr = btn.querySelector<HTMLElement>('.addr') ?? btn;
  if (value) {
    addr.textContent = value;
    addr.title = value;
    btn.dataset.full = value;
    btn.disabled = false;
  } else {
    addr.textContent = 'after the first connect';
    addr.title = '';
    delete btn.dataset.full;
    btn.disabled = true;
  }
}

// EVM chain picker is static
for (const c of EVM_CHAINS) {
  const o = document.createElement('option');
  o.value = String(c.id);
  o.textContent = `${c.name} · ${c.id}`;
  ui.evmChain.append(o);
}

function render(): void {
  if (!state) return;
  ui.status.dataset.state = state.socket;
  ui.statusText.textContent =
    state.lastError && state.socket !== 'open' ? `${STATUS_LABEL[state.socket]} · ${state.lastError}` : STATUS_LABEL[state.socket];
  ui.statusText.title = state.lastError ?? '';

  if (!hostDirty) ui.host.value = state.daemonUrl || DEFAULT_DAEMON_URL;
  if (!rpcDirty) ui.rpc.value = state.rpcUrl || DEFAULT_RPC_URL;
  if (!btcDirty) ui.btcApi.value = state.btcApiUrl || DEFAULT_BTC_API_URL;

  // EVM chain + per-chain RPC
  const chainShown = shownChainId ?? state.evmChainId;
  ui.evmChain.value = String(chainShown);
  ui.evmChainId.textContent = chainShown === state.evmChainId ? `active · ${chainIdHex(state.evmChainId)}` : `editing RPC · active is ${state.evmChainId}`;
  if (!evmRpcDirty) ui.evmRpc.value = state.evmRpcUrls[String(chainShown)] ?? '';
  ui.takeover.checked = state.takeoverEthereum;

  ui.pairBlock.hidden = state.paired;
  ui.pairedBlock.hidden = !state.paired;
  if (state.paired) {
    renderAddress(ui.addrSolana, state.addresses.solana ?? state.pubkey ?? undefined);
    renderAddress(ui.addrEvm, state.addresses.evm);
    renderAddress(ui.addrBitcoin, state.addresses.bitcoin);
    ui.addrHint.hidden = Boolean(state.addresses.evm && state.addresses.bitcoin);
    ui.origins.textContent = state.approvedOrigins.length ? String(state.approvedOrigins.length) : '';
    ui.sitesEmpty.hidden = state.approvedOrigins.length > 0;
    renderSites(state.approvedOrigins, state.connectedOrigins);
  }
  renderApprovals(state.pendingApprovals);

  const p = state.pending;
  ui.pendingCard.hidden = !p;
  ui.idleCard.hidden = Boolean(p) || state.pendingApprovals.length > 0;
  ui.idleHint.textContent = state.paired
    ? 'The robot is watching the room. A request from a connected site shows up here, and on the robot.'
    : 'Pair with your robot below; requests from sites show up here once it is connected.';
  if (p) {
    const chain = chainOf(p);
    ui.pendingChain.textContent = CHAIN_LABEL[chain] ?? chain;
    ui.pendingChain.dataset.chain = chain;
    ui.pendingKind.textContent = KIND_LABEL[p.kind] ?? p.kind;
    ui.pendingOrigin.textContent = originLabel(p.origin);
    ui.pendingSiteMark.textContent = originLabel(p.origin).replace(/^www\./, '').slice(0, 1) || '?';
    if (p.explain) {
      ui.pendingRisk.textContent = RISK_LABEL[p.explain.risk] ?? p.explain.risk;
      ui.pendingRisk.dataset.risk = p.explain.risk;
      ui.pendingSummary.textContent = p.explain.summary;
      list(ui.pendingReasons, p.explain.reasons);
      rows(ui.pendingLines, p.explain.lines);
    } else {
      ui.pendingRisk.textContent = RISK_LABEL.none;
      ui.pendingRisk.dataset.risk = 'none';
      const what =
        p.kind === 'signMessage' || p.kind === 'evm_signMessage' || p.kind === 'btc_signMessage'
          ? 'message'
          : p.kind === 'evm_signTypedData'
            ? 'typed data'
            : p.kind === 'btc_signPsbt'
              ? 'PSBT'
              : 'transaction';
      ui.pendingSummary.textContent = `The robot is reading the ${what}…`;
      list(ui.pendingReasons, []);
      rows(ui.pendingLines, []);
    }
    ui.pendingTimer.textContent = ago(p.startedAt);
  }

  renderLog(state.log);
}

function siteButton(text: string, cls: string, onClick: () => void): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = cls;
  b.textContent = text;
  b.addEventListener('click', onClick);
  return b;
}

function renderApprovals(origins: string[]): void {
  ui.approvalCard.hidden = origins.length === 0;
  ui.approvals.replaceChildren(
    ...origins.map((origin) => {
      const li = document.createElement('li');
      const site = document.createElement('span');
      site.className = 'site';
      site.textContent = originLabel(origin);
      site.title = origin;
      li.append(
        site,
        siteButton('Deny', 'small', () => send({ t: 'denyOrigin', origin })),
        siteButton('Approve', 'small accent', () => send({ t: 'approveOrigin', origin })),
      );
      return li;
    }),
  );
}

function renderSites(origins: string[], connected: string[] = []): void {
  ui.sites.replaceChildren(
    ...origins.map((origin) => {
      const li = document.createElement('li');
      const site = document.createElement('span');
      site.className = 'site';
      site.textContent = originLabel(origin);
      site.title = origin;
      li.append(site);
      if (connected.includes(origin)) {
        const live = document.createElement('span');
        live.className = 'live';
        live.textContent = 'open now';
        li.append(live);
      }
      li.append(siteButton('Remove', 'small ghost', () => send({ t: 'removeOrigin', origin })));
      return li;
    }),
  );
}

function renderLog(log: LogEntry[]): void {
  ui.logEmpty.hidden = log.length > 0;
  ui.logCount.textContent = log.length ? `${log.length}` : '';
  ui.log.replaceChildren(
    ...log.map((e) => {
      const chain = chainOf(e);
      const li = document.createElement('li');
      const g = document.createElement('span');
      g.className = 'glyph';
      g.dataset.chain = chain;
      g.innerHTML = `<svg><use href="#g-${chain}"/></svg>`;
      const what = document.createElement('span');
      what.className = 'what';
      const s = document.createElement('span');
      s.className = 's';
      s.textContent = e.summary ?? KIND_LABEL[e.kind] ?? e.kind;
      const m = document.createElement('span');
      m.className = 'm';
      m.textContent = [originLabel(e.origin), e.risk ? RISK_LABEL[e.risk] ?? e.risk : '', e.detail].filter(Boolean).join(' · ');
      what.append(s, m);
      const side = document.createElement('span');
      side.className = 'side';
      const o = document.createElement('span');
      o.className = 'o';
      o.dataset.outcome = e.outcome;
      o.textContent = e.outcome;
      const t = document.createElement('span');
      t.className = 't';
      t.textContent = ago(e.ts);
      side.append(o, t);
      li.append(g, what, side);
      return li;
    }),
  );
}

// views: home / activity / settings, from the tab bar and the gear
function setView(name: string): void {
  for (const v of document.querySelectorAll<HTMLElement>('.view')) v.hidden = v.dataset.view !== name;
  for (const b of document.querySelectorAll<HTMLElement>('.tab')) b.classList.toggle('is-active', b.dataset.view === name);
  document.querySelector('main')?.scrollTo({ top: 0 });
}
for (const b of document.querySelectorAll<HTMLElement>('[data-view]')) {
  if (b.classList.contains('view')) continue;
  b.addEventListener('click', () => setView(b.dataset.view ?? 'home'));
}

// keep the pending timer and "ago" labels moving while the popup is open
setInterval(() => {
  if (state?.pending) ui.pendingTimer.textContent = ago(state.pending.startedAt);
}, 1000);

ui.host.addEventListener('input', () => (hostDirty = true));
ui.rpc.addEventListener('input', () => (rpcDirty = true));
ui.evmRpc.addEventListener('input', () => (evmRpcDirty = true));
ui.btcApi.addEventListener('input', () => (btcDirty = true));

ui.hostForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  hostDirty = false;
  send({ t: 'setDaemonUrl', url: ui.host.value });
});

ui.rpcForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  rpcDirty = false;
  send({ t: 'setRpcUrl', url: ui.rpc.value });
});

ui.btcForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  btcDirty = false;
  send({ t: 'setBtcApiUrl', url: ui.btcApi.value });
});

ui.evmChain.addEventListener('change', () => {
  const id = Number(ui.evmChain.value);
  shownChainId = id;
  evmRpcDirty = false;
  // picking a chain in the popup also makes it the wallet's active chain (dapps get chainChanged)
  send({ t: 'setEvmChain', chainId: id });
  render();
});

ui.evmRpcForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  evmRpcDirty = false;
  send({ t: 'setEvmRpcUrl', chainId: Number(ui.evmChain.value), url: ui.evmRpc.value });
});

ui.takeover.addEventListener('change', () => {
  send({ t: 'setTakeoverEthereum', on: ui.takeover.checked });
});

ui.pairForm.addEventListener('submit', (ev) => {
  ev.preventDefault();
  const code = ui.code.value.replace(/\D/g, '');
  if (code.length !== 6) {
    toast('the pairing code is 6 digits', 'err');
    return;
  }
  send({ t: 'pair', code });
  ui.code.value = '';
});

ui.code.addEventListener('input', () => {
  ui.code.value = ui.code.value.replace(/\D/g, '').slice(0, 6);
});

ui.reconnect.addEventListener('click', () => send({ t: 'reconnect' }));

ui.forget.addEventListener('click', () => {
  if (confirm('Forget this robot? The token is revoked on the robot, every site approval is dropped, and you will need a new pairing code.')) {
    send({ t: 'forget' });
  }
});

for (const btn of [ui.addrSolana, ui.addrEvm, ui.addrBitcoin]) {
  btn.addEventListener('click', async () => {
    const full = btn.dataset.full;
    if (!full) return;
    try {
      await navigator.clipboard.writeText(full);
      toast(`copied ${btn.dataset.chain} ${shortKey(full)}`, 'ok');
    } catch {
      toast('clipboard blocked', 'err');
    }
  });
}

if (DEMO) {
  const now = Date.now();
  const scene = new URLSearchParams(location.search).get('demo') || 'pending';
  state = {
    socket: 'open',
    lastError: null,
    daemonUrl: 'ws://127.0.0.1:7401',
    rpcUrl: '',
    btcApiUrl: '',
    evmChainId: 1,
    evmRpcUrls: {},
    takeoverEthereum: false,
    paired: true,
    pubkey: '2MXsi6TVF3x2cEZKk6d1eEgPornPQJUaa59jTG7AVJPu',
    addresses: {
      solana: '2MXsi6TVF3x2cEZKk6d1eEgPornPQJUaa59jTG7AVJPu',
      evm: '0x24eC70214230AE62F4cD13553da38caa8D711231',
      bitcoin: 'bc1qyqea6dngw90nqwpxepmy4v3mgk53hp6zxup02a',
    },
    connectedOrigins: ['http://localhost:8765'],
    approvedOrigins: ['http://localhost:8765'],
    pendingApprovals: scene === 'ask' ? ['https://jup.ag'] : [],
    pending:
      scene === 'pending'
        ? {
            id: 'd1',
            kind: 'signAndSendTransaction',
            chain: 'solana',
            origin: 'http://localhost:8765',
            startedAt: now - 7000,
            explain: {
              risk: 'low',
              summary: '0.01 SOL to 9xQe…VFin',
              reasons: ['First time you send to this address.'],
              lines: ['fee 0.000005 SOL', 'devnet'],
            },
          }
        : null,
    log: [
      { ts: now - 3600e3, kind: 'signAndSendTransaction', chain: 'solana', origin: 'http://localhost:8765', outcome: 'sent', summary: '0.05 SOL to 9xQe…VFin', risk: 'low' },
      { ts: now - 86400e3, kind: 'evm_signTypedData', chain: 'evm', origin: 'https://app.uniswap.org', outcome: 'denied', summary: 'Permit USDC, unlimited', risk: 'high' },
    ],
  } as unknown as StateView;
  render();
}
