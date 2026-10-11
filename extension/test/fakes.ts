import type { MessageEventLike, WindowLike } from '../src/lib/pageTransport';
import type { PortLike } from '../src/lib/contentBridge';
import type { SocketLike, StorageLike } from '../src/background/core';
import type { Settings } from '../src/protocol';

/**
 * Minimal `window` with browser-like postMessage semantics (async delivery, targetOrigin check)
 * plus a plain DOM-style event target for everything else (EIP-6963 events).
 */
export class FakeWindow implements WindowLike {
  readonly location: { origin: string };
  readonly listeners = new Set<(ev: MessageEventLike) => void>();
  readonly posted: Array<{ message: unknown; targetOrigin: string }> = [];
  readonly domListeners = new Map<string, Set<(ev: unknown) => void>>();
  readonly dispatched: Event[] = [];
  btc_providers?: unknown;

  constructor(origin = 'https://pump.fun') {
    this.location = { origin };
  }

  postMessage(message: unknown, targetOrigin: string): void {
    this.posted.push({ message, targetOrigin });
    if (targetOrigin !== '*' && targetOrigin !== this.location.origin) return; // browser drops it
    this.deliver({ source: this, origin: this.location.origin, data: structuredClone(message) });
  }

  /** Deliver an arbitrary event, e.g. one forged from another window or origin. */
  deliver(ev: MessageEventLike): void {
    queueMicrotask(() => {
      for (const l of [...this.listeners]) l(ev);
    });
  }

  addEventListener(type: 'message', listener: (ev: MessageEventLike) => void): void;
  addEventListener(type: string, listener: (ev: unknown) => void): void;
  addEventListener(type: string, listener: (ev: MessageEventLike) => void): void {
    if (type === 'message') {
      this.listeners.add(listener as (ev: MessageEventLike) => void);
      return;
    }
    let set = this.domListeners.get(type);
    if (!set) this.domListeners.set(type, (set = new Set()));
    set.add(listener as (ev: unknown) => void);
  }

  removeEventListener(type: 'message', listener: (ev: MessageEventLike) => void): void;
  removeEventListener(type: string, listener: (ev: unknown) => void): void;
  removeEventListener(type: string, listener: (ev: MessageEventLike) => void): void {
    if (type === 'message') this.listeners.delete(listener as (ev: MessageEventLike) => void);
    else this.domListeners.get(type)?.delete(listener as (ev: unknown) => void);
  }

  /** Synchronous, like the DOM. */
  dispatchEvent(ev: Event): boolean {
    this.dispatched.push(ev);
    for (const l of [...(this.domListeners.get(ev.type) ?? [])]) l(ev);
    return true;
  }
}

export class FakePort implements PortLike {
  readonly sent: unknown[] = [];
  private readonly msgListeners: Array<(m: unknown) => void> = [];
  private readonly discListeners: Array<() => void> = [];
  disconnected = false;

  postMessage(msg: unknown): void {
    if (this.disconnected) throw new Error('Attempting to use a disconnected port object');
    this.sent.push(msg);
  }
  onMessage(l: (m: unknown) => void): void {
    this.msgListeners.push(l);
  }
  onDisconnect(l: () => void): void {
    this.discListeners.push(l);
  }
  disconnect(): void {
    this.disconnected = true;
  }
  /** background → content */
  receive(msg: unknown): void {
    for (const l of this.msgListeners) l(msg);
  }
  /** simulate the service worker dying */
  kill(): void {
    this.disconnected = true;
    for (const l of this.discListeners) l();
  }
}

export class MockSocket implements SocketLike {
  readonly sent: string[] = [];
  closed = false;
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;

  constructor(readonly url: string) {}

  send(d: string): void {
    if (this.closed) throw new Error('socket closed');
    this.sent.push(d);
  }
  close(): void {
    this.closed = true;
  }
  open(): void {
    this.onopen?.();
  }
  /** daemon → extension, newline-delimited like the real link */
  receive(obj: unknown): void {
    this.onmessage?.({ data: JSON.stringify(obj) + '\n' });
  }
  drop(): void {
    this.closed = true;
    this.onclose?.();
  }
  /** parsed frames the extension sent */
  get frames(): Array<Record<string, unknown>> {
    return this.sent.flatMap((s) =>
      s
        .split('\n')
        .filter(Boolean)
        .map((l) => JSON.parse(l) as Record<string, unknown>),
    );
  }
}

export class MemoryStorage implements StorageLike {
  data: Partial<Settings> = {};
  async get(): Promise<Partial<Settings>> {
    return structuredClone(this.data);
  }
  async set(patch: Partial<Settings>): Promise<void> {
    for (const [k, v] of Object.entries(patch)) {
      if (v === undefined) delete (this.data as Record<string, unknown>)[k];
      else (this.data as Record<string, unknown>)[k] = structuredClone(v);
    }
  }
}

/** Drain the microtask queue (safe under vi.useFakeTimers, which freezes setTimeout). */
export const flush = async (): Promise<void> => {
  for (let i = 0; i < 25; i++) await Promise.resolve();
};
