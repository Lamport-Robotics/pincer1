import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BackgroundCore, CoreError } from '../src/background/core';
import { PAGE_CHANNEL, type PageRequest, type Settings, type StateView } from '../src/protocol';
import { fromBase64 } from '../src/lib/bytes';
import { MemoryStorage, MockSocket, flush } from './fakes';

const ORIGIN = 'https://pump.fun';
const TX = 'AQIDBA=='; // [1,2,3,4]

function req<M extends PageRequest['method']>(method: M, params: PageRequest<M>['params']): PageRequest<M> {
  return { channel: PAGE_CHANNEL, dir: 'page→ext', id: `req-${method}`, method, params };
}

function harness(initial: Partial<Settings> = {}) {
  const storage = new MemoryStorage();
  Object.assign(storage.data, initial);
  const sockets: MockSocket[] = [];
  const badges: string[] = [];
  const states: StateView[] = [];
  const sendRaw = vi.fn(async (_rpc: string, _tx: Uint8Array) => 'SIG'.repeat(10));
  const core = new BackgroundCore({
    createSocket: (url) => {
      const s = new MockSocket(url);
      sockets.push(s);
      return s;
    },
    storage,
    setBadge: (t) => badges.push(t),
    sendRawTransaction: sendRaw,
  });
  core.onState((s) => states.push(s));
  return { core, storage, sockets, badges, states, sendRaw, sock: () => sockets.at(-1)! };
}

/** Bring a fresh core to "paired + socket open" using the real pair handshake. */
async function pairedCore() {
  const h = harness();
  await h.core.init();
  h.sock().open();
  const pairing = h.core.pair('123456');
  await flush();
  h.sock().receive({ t: 'paired', token: 'tok-1', pubkey: 'PUBKEY1' });
  await pairing;
  return h;
}

describe('BackgroundCore', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('pair → connect → signTransaction → explain → signed', async () => {
    const h = harness();
    await h.core.init();
    expect(h.sockets).toHaveLength(1);
    expect(h.sock().url).toBe('ws://pincer1.local:7401');
    expect(h.core.getState().socket).toBe('connecting');

    h.sock().open();
    expect(h.core.getState().socket).toBe('open');

    // pair
    const pairing = h.core.pair('12 34 56');
    await flush();
    expect(h.sock().frames).toEqual([{ t: 'pair', code: '123456' }]);
    h.sock().receive({ t: 'paired', token: 'tok-1', pubkey: 'PUBKEY1' });
    expect(await pairing).toEqual({ pubkey: 'PUBKEY1' });
    expect(h.storage.data.token).toBe('tok-1');
    expect(h.storage.data.pubkey).toBe('PUBKEY1');
    expect(h.core.getState()).toMatchObject({ paired: true, pubkey: 'PUBKEY1' });

    // connect from a dapp
    const connecting = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    expect(h.sock().frames.at(-1)).toEqual({ t: 'connect', token: 'tok-1', origin: ORIGIN });
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    expect(await connecting).toEqual({ pubkey: 'PUBKEY1' });
    expect(h.core.getState().connectedOrigins).toEqual([ORIGIN]);

    // a second connect from the same origin is answered locally
    expect(await h.core.handlePageRequest(ORIGIN, req('connect', { silent: true }))).toEqual({ pubkey: 'PUBKEY1' });
    expect(h.sock().frames.filter((f) => f.t === 'connect')).toHaveLength(1);

    // sign
    const signing = h.core.handlePageRequest(ORIGIN, req('signTransaction', { tx: TX, chain: 'solana:mainnet' }));
    await flush();
    const sent = h.sock().frames.at(-1)!;
    expect(sent).toMatchObject({ t: 'signTransaction', tx: TX, origin: ORIGIN });
    const id = sent.id as string;
    expect(typeof id).toBe('string');
    expect(h.badges.at(-1)).toBe('1');
    expect(h.core.getState().pending).toMatchObject({ id, kind: 'signTransaction', origin: ORIGIN, explain: null });

    h.sock().receive({
      t: 'explain',
      id,
      summary: '0.5 SOL to a 3-day-old address',
      risk: 'medium',
      reasons: ['fresh destination', 'no prior interaction'],
    });
    expect(h.core.getState().pending?.explain).toEqual({
      summary: '0.5 SOL to a 3-day-old address',
      risk: 'medium',
      reasons: ['fresh destination', 'no prior interaction'],
    });

    h.sock().receive({ t: 'signed', id, tx: 'c2lnbmVk' });
    expect(await signing).toEqual({ tx: 'c2lnbmVk' });
    expect(h.badges.at(-1)).toBe('');
    const state = h.core.getState();
    expect(state.pending).toBeNull();
    expect(state.log[0]).toMatchObject({
      id,
      kind: 'signTransaction',
      origin: ORIGIN,
      outcome: 'signed',
      summary: '0.5 SOL to a 3-day-old address',
      risk: 'medium',
    });
    expect(h.storage.data.log?.[0]?.outcome).toBe('signed');
  });

  it('signMessage returns the daemon signature', async () => {
    const h = await pairedCore();
    const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await c;
    const p = h.core.handlePageRequest(ORIGIN, req('signMessage', { message: 'aGVsbG8=' }));
    await flush();
    const sent = h.sock().frames.at(-1)!;
    expect(sent).toMatchObject({ t: 'signMessage', message: 'aGVsbG8=' });
    h.sock().receive({ t: 'signed', id: sent.id, signature: 'c2ln' });
    expect(await p).toEqual({ signature: 'c2ln' });
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'signMessage', outcome: 'signed' });
  });

  it('denied path: rejects with DENIED and the daemon reason, logs it', async () => {
    const h = await pairedCore();
    // signing without an explicit connect runs the handshake transparently
    const signing = h.core.handlePageRequest(ORIGIN, req('signTransaction', { tx: TX }));
    await flush();
    expect(h.sock().frames.at(-1)).toMatchObject({ t: 'connect', origin: ORIGIN });
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await flush();
    const sent = h.sock().frames.at(-1)!;
    expect(sent.t).toBe('signTransaction');
    h.sock().receive({ t: 'explain', id: sent.id, summary: 'drains the wallet', risk: 'block', reasons: ['known drainer'] });
    h.sock().receive({ t: 'denied', id: sent.id, reason: 'policy' });
    const err = await signing.catch((e) => e);
    expect(err).toBeInstanceOf(CoreError);
    expect(err).toMatchObject({ code: 'DENIED', message: 'policy' });
    expect(h.core.getState().pending).toBeNull();
    expect(h.badges.at(-1)).toBe('');
    expect(h.core.getState().log[0]).toMatchObject({ outcome: 'denied', risk: 'block', summary: 'drains the wallet', detail: 'policy' });
  });

  it('timeout path: gives up after 90 s and clears the badge', async () => {
    const h = await pairedCore();
    const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await c;
    const outcome = h.core.handlePageRequest(ORIGIN, req('signTransaction', { tx: TX })).catch((e) => e);
    await flush();
    const sent = h.sock().frames.at(-1)!;
    h.sock().receive({ t: 'explain', id: sent.id, summary: 'ok', risk: 'low', reasons: [] });
    expect(h.badges.at(-1)).toBe('1');
    await vi.advanceTimersByTimeAsync(89_999);
    expect(h.core.getState().pending).not.toBeNull();
    await vi.advanceTimersByTimeAsync(1);
    const err = await outcome;
    expect(err).toMatchObject({ code: 'TIMEOUT' });
    expect(h.badges.at(-1)).toBe('');
    expect(h.core.getState().log[0]).toMatchObject({ outcome: 'timeout', summary: 'ok' });
    // a late "signed" for a dead request is ignored
    h.sock().receive({ t: 'signed', id: sent.id, tx: 'late' });
    expect(h.core.getState().log).toHaveLength(1);
  });

  it('signAndSendTransaction signs via the robot then broadcasts with the configured RPC', async () => {
    const h = await pairedCore();
    await h.core.setRpcUrl('https://rpc.example/');
    const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await c;
    const p = h.core.handlePageRequest(
      ORIGIN,
      req('signAndSendTransaction', { tx: TX, chain: 'solana:mainnet', options: { skipPreflight: true } }),
    );
    await flush();
    const sent = h.sock().frames.at(-1)!;
    expect(sent.t).toBe('signTransaction'); // §3 has no signAndSend; the extension sends
    h.sock().receive({ t: 'signed', id: sent.id, tx: 'c2lnbmVk' });
    expect(await p).toEqual({ signature: 'SIG'.repeat(10) });
    expect(h.sendRaw).toHaveBeenCalledWith('https://rpc.example/', fromBase64('c2lnbmVk'), { skipPreflight: true });
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'signAndSendTransaction', outcome: 'signed', detail: 'sent SIGSIGSI…' });

    // devnet never uses the mainnet RPC
    const p2 = h.core.handlePageRequest(ORIGIN, req('signAndSendTransaction', { tx: TX, chain: 'solana:devnet' }));
    await flush();
    h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, tx: 'c2lnbmVk' });
    await p2;
    expect(h.sendRaw).toHaveBeenLastCalledWith('https://api.devnet.solana.com', expect.anything(), undefined);
  });

  it('refuses to connect when not paired and refuses a pairing with the wrong shape', async () => {
    const h = harness();
    await h.core.init();
    h.sock().open();
    await expect(h.core.handlePageRequest(ORIGIN, req('connect', {}))).rejects.toMatchObject({ code: 'NOT_PAIRED' });
    await expect(h.core.pair('12')).rejects.toMatchObject({ code: 'BAD_REQUEST' });
    const pairing = h.core.pair('654321');
    await flush();
    h.sock().receive({ t: 'denied', reason: 'bad code' });
    await expect(pairing).rejects.toMatchObject({ code: 'DENIED', message: 'bad code' });
    expect(h.core.getState().paired).toBe(false);
  });

  it('reconnects with exponential backoff and fails in-flight requests when the socket drops', async () => {
    const h = await pairedCore();
    const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await c;
    const outcome = h.core.handlePageRequest(ORIGIN, req('signTransaction', { tx: TX })).catch((e) => e);
    await flush();
    const first = h.sock();
    first.drop();
    expect(await outcome).toMatchObject({ code: 'ROBOT_OFFLINE' });
    expect(h.core.getState()).toMatchObject({ socket: 'reconnecting', connectedOrigins: [], paired: true });
    expect(h.sockets).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1000);
    expect(h.sockets).toHaveLength(2);
    h.sock().drop();
    await vi.advanceTimersByTimeAsync(1999);
    expect(h.sockets).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(1);
    expect(h.sockets).toHaveLength(3);
    h.sock().open();
    expect(h.core.getState().socket).toBe('open');
    // after a reconnect the origin must re-handshake (the daemon forgot it too)
    const again = h.core.handlePageRequest(ORIGIN, req('signTransaction', { tx: TX }));
    await flush();
    expect(h.sock().frames.at(-1)).toMatchObject({ t: 'connect', token: 'tok-1', origin: ORIGIN });
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await flush();
    h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, tx: 'ok' });
    expect(await again).toEqual({ tx: 'ok' });
  });

  it('forget clears the token, tells pages to disconnect, and keeps the socket', async () => {
    const h = await pairedCore();
    const events: string[] = [];
    h.core.onPageEvent((e) => events.push(e));
    await h.core.forget();
    expect(events).toEqual(['disconnected']);
    expect(h.storage.data.token).toBeUndefined();
    expect(h.core.getState()).toMatchObject({ paired: false, pubkey: null, socket: 'open' });
  });

  it('setDaemonUrl validates, persists, and reopens the socket', async () => {
    const h = harness();
    await h.core.init();
    await expect(h.core.setDaemonUrl('http://nope')).rejects.toMatchObject({ code: 'BAD_REQUEST' });
    await h.core.setDaemonUrl('ws://192.168.1.50:7401/');
    expect(h.storage.data.daemonUrl).toBe('ws://192.168.1.50:7401');
    expect(h.sockets).toHaveLength(2);
    expect(h.sock().url).toBe('ws://192.168.1.50:7401');
    expect(h.sockets[0]!.closed).toBe(true);
  });

  it('keeps only the last 20 log entries', async () => {
    const h = await pairedCore();
    const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: 'PUBKEY1' });
    await c;
    for (let i = 0; i < 25; i++) {
      const p = h.core.handlePageRequest(ORIGIN, req('signMessage', { message: 'AA==' }));
      await flush();
      h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, signature: 'AA==' });
      await p;
    }
    expect(h.core.getState().log).toHaveLength(20);
    expect(h.storage.data.log).toHaveLength(20);
  });
});
