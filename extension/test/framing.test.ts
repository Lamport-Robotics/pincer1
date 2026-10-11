import { describe, expect, it } from 'vitest';
import { PageTransport, PincerError } from '../src/lib/pageTransport';
import { createContentBridge } from '../src/lib/contentBridge';
import { PAGE_CHANNEL, isPageRequest, type PageReply, type PageRequest } from '../src/protocol';
import { FakePort, FakeWindow, flush } from './fakes';

const reply = (id: string, result: unknown): PageReply => ({ channel: PAGE_CHANNEL, dir: 'ext→page', id, ok: true, result });

/** Stand-in for the content script: answers page→ext frames on the same window. */
function fakeContentScript(win: FakeWindow, handler: (req: PageRequest) => PageReply | null): void {
  win.addEventListener('message', (ev) => {
    if (ev.source !== win || ev.origin !== win.location.origin) return;
    if (!isPageRequest(ev.data)) return;
    const r = handler(ev.data);
    if (r) win.postMessage(r, win.location.origin);
  });
}

describe('PageTransport (page → content framing)', () => {
  it('matches a reply to its request by id and resolves the result', async () => {
    const win = new FakeWindow();
    const t = new PageTransport(win);
    fakeContentScript(win, (req) => reply(req.id, { pubkey: 'A'.repeat(32) }));

    const res = await t.request('connect', {});
    expect(res).toEqual({ pubkey: 'A'.repeat(32) });
    expect(t.inflight).toBe(0);

    const sent = win.posted[0]!.message as PageRequest;
    expect(sent.channel).toBe('pincer1');
    expect(sent.dir).toBe('page→ext');
    expect(sent.method).toBe('connect');
    expect(typeof sent.id).toBe('string');
    expect(win.posted[0]!.targetOrigin).toBe('https://pump.fun');
  });

  it('keeps concurrent requests apart even when replies arrive out of order', async () => {
    const win = new FakeWindow();
    const t = new PageTransport(win);
    const queued: PageRequest[] = [];
    fakeContentScript(win, (req) => {
      queued.push(req);
      return null;
    });
    const a = t.request('signMessage', { message: 'YQ==' });
    const b = t.request('signMessage', { message: 'Yg==' });
    await flush();
    expect(queued).toHaveLength(2);
    expect(queued[0]!.id).not.toBe(queued[1]!.id);
    // answer b first
    win.postMessage(reply(queued[1]!.id, { signature: 'sig-b' }), win.location.origin);
    win.postMessage(reply(queued[0]!.id, { signature: 'sig-a' }), win.location.origin);
    expect(await a).toEqual({ signature: 'sig-a' });
    expect(await b).toEqual({ signature: 'sig-b' });
  });

  it('rejects with the error code from an error reply', async () => {
    const win = new FakeWindow();
    const t = new PageTransport(win);
    fakeContentScript(win, (req) => ({
      channel: PAGE_CHANNEL,
      dir: 'ext→page',
      id: req.id,
      ok: false,
      error: { code: 'DENIED', reason: 'user' } as never,
    }));
    const err = await t.request('signTransaction', { tx: 'AA==' }).catch((e) => e);
    expect(err).toBeInstanceOf(PincerError);
    expect((err as PincerError).code).toBe('DENIED');
  });

  it('ignores replies from another window, another origin, an unknown id, or another channel', async () => {
    const win = new FakeWindow();
    const t = new PageTransport(win);
    let pendingId = '';
    fakeContentScript(win, (req) => {
      pendingId = req.id;
      return null;
    });
    const p = t.request('connect', {});
    await flush();
    expect(pendingId).not.toBe('');

    const forged = reply(pendingId, { pubkey: 'EVIL' });
    // 1. same data, but posted by an iframe (different source)
    win.deliver({ source: {}, origin: win.location.origin, data: forged });
    // 2. same window object, but a different origin on the event
    win.deliver({ source: win, origin: 'https://evil.example', data: forged });
    // 3. right source + origin, wrong id
    win.deliver({ source: win, origin: win.location.origin, data: reply('nope', { pubkey: 'EVIL' }) });
    // 4. right everything, wrong channel
    win.deliver({ source: win, origin: win.location.origin, data: { ...forged, channel: 'other' } });
    // 5. a page→ext frame echoed back must not be treated as a reply
    win.deliver({ source: win, origin: win.location.origin, data: { ...forged, dir: 'page→ext' } });
    await flush();
    expect(t.inflight).toBe(1);

    win.postMessage(reply(pendingId, { pubkey: 'GOOD' }), win.location.origin);
    expect(await p).toEqual({ pubkey: 'GOOD' });
  });

  it('dispatches unsolicited events to event listeners', async () => {
    const win = new FakeWindow();
    const t = new PageTransport(win);
    const seen: string[] = [];
    t.onEvent((e) => seen.push(e));
    win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', event: 'disconnected' }, win.location.origin);
    await flush();
    expect(seen).toEqual(['disconnected']);
  });
});

describe('content bridge (content ↔ background port)', () => {
  it('forwards only same-window, same-origin page→ext frames and relays port replies back', async () => {
    const win = new FakeWindow();
    const ports: FakePort[] = [];
    const bridge = createContentBridge({
      win,
      connect: () => {
        const p = new FakePort();
        ports.push(p);
        return p;
      },
    });
    const t = new PageTransport(win);
    const p = t.request('connect', { silent: true });
    await flush();
    expect(ports).toHaveLength(1);
    expect(ports[0]!.sent).toHaveLength(1);
    const forwarded = ports[0]!.sent[0] as PageRequest;
    expect(forwarded.method).toBe('connect');

    // junk that must not be forwarded
    win.deliver({ source: {}, origin: win.location.origin, data: forwarded }); // other source
    win.deliver({ source: win, origin: 'https://evil.example', data: forwarded }); // other origin
    win.deliver({ source: win, origin: win.location.origin, data: { hello: 1 } }); // not a frame
    win.deliver({ source: win, origin: win.location.origin, data: reply('x', {}) }); // ext→page direction
    await flush();
    expect(bridge.forwarded).toBe(1);
    expect(ports[0]!.sent).toHaveLength(1);

    ports[0]!.receive(reply(forwarded.id, { pubkey: 'PK' }));
    expect(await p).toEqual({ pubkey: 'PK' });
    bridge.dispose();
  });

  it('opens a fresh port after the service worker drops the old one', async () => {
    const win = new FakeWindow();
    const ports: FakePort[] = [];
    createContentBridge({
      win,
      connect: () => {
        const p = new FakePort();
        ports.push(p);
        return p;
      },
    });
    const t = new PageTransport(win);
    void t.request('disconnect', {});
    await flush();
    expect(ports).toHaveLength(1);
    ports[0]!.kill();
    void t.request('disconnect', {});
    await flush();
    expect(ports).toHaveLength(2);
    expect(ports[1]!.sent).toHaveLength(1);
  });

  it('ignores port messages that are not ext→page frames', async () => {
    const win = new FakeWindow();
    const port = new FakePort();
    createContentBridge({ win, connect: () => port });
    const t = new PageTransport(win);
    void t.request('connect', {});
    await flush();
    const before = win.posted.length;
    port.receive({ random: true });
    port.receive('string');
    await flush();
    expect(win.posted.length).toBe(before);
  });
});
