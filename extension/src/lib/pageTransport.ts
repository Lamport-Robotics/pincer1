/**
 * Page-world side of the page ⇄ content-script bridge.
 *
 * Framing: `window.postMessage({channel:'pincer1', dir:'page→ext', id, method, params}, origin)`.
 * Replies come back as `{channel, dir:'ext→page', id, ok, result|error}`. Every inbound frame is
 * accepted only if it was posted by this same window (`event.source === window`) from this same
 * origin — that rules out iframes and cross-origin windows injecting fake replies.
 */
import {
  PAGE_CHANNEL,
  isPageReply,
  type PageEvent,
  type PageEventName,
  type PageMethod,
  type PageParams,
  type PageReply,
  type PageRequest,
  type PageResults,
  type WalletError,
} from '../protocol';
import { newId } from './bytes';

/** The subset of `window` we touch, so tests can pass a fake. */
export interface WindowLike {
  postMessage(message: unknown, targetOrigin: string): void;
  addEventListener(type: 'message', listener: (ev: MessageEventLike) => void): void;
  removeEventListener(type: 'message', listener: (ev: MessageEventLike) => void): void;
  location: { origin: string };
}

export interface MessageEventLike {
  source: unknown;
  origin: string;
  data: unknown;
}

export class PincerError extends Error implements WalletError {
  constructor(
    public readonly code: WalletError['code'],
    message: string,
    /** For RPC_ERROR: the upstream JSON-RPC error (code/data) so providers can pass it through. */
    public readonly rpc?: WalletError['rpc'],
  ) {
    super(message);
    this.name = 'PincerError';
  }
}

/** Unsolicited events: `(name, payload)` — the payload carries e.g. the new chainId. */
export type EventListener = (name: PageEventName, payload: PageEvent) => void;

interface Waiter {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
}

export class PageTransport {
  private readonly waiters = new Map<string, Waiter>();
  private readonly eventListeners = new Set<EventListener>();
  private readonly onMessage = (ev: MessageEventLike): void => this.handle(ev);

  constructor(
    private readonly win: WindowLike,
    private readonly idPrefix = 'p',
  ) {
    win.addEventListener('message', this.onMessage);
  }

  get origin(): string {
    return this.win.location.origin;
  }

  request<M extends PageMethod>(method: M, params: PageParams[M]): Promise<PageResults[M]> {
    const id = newId(this.idPrefix);
    const frame: PageRequest<M> = { channel: PAGE_CHANNEL, dir: 'page→ext', id, method, params };
    return new Promise<PageResults[M]>((resolve, reject) => {
      this.waiters.set(id, { resolve: resolve as (v: unknown) => void, reject });
      try {
        this.win.postMessage(frame, this.origin);
      } catch (e) {
        this.waiters.delete(id);
        reject(new PincerError('INTERNAL', (e as Error).message));
      }
    });
  }

  onEvent(listener: EventListener): () => void {
    this.eventListeners.add(listener);
    return () => this.eventListeners.delete(listener);
  }

  /** Number of in-flight requests (for tests). */
  get inflight(): number {
    return this.waiters.size;
  }

  dispose(): void {
    this.win.removeEventListener('message', this.onMessage);
    for (const w of this.waiters.values()) w.reject(new PincerError('INTERNAL', 'transport disposed'));
    this.waiters.clear();
  }

  private handle(ev: MessageEventLike): void {
    if (ev.source !== this.win) return;
    if (ev.origin !== this.origin) return;
    const data = ev.data;
    if (!isPageReply(data)) return;
    const reply: PageReply = data;
    if ('event' in reply) {
      const { channel: _c, dir: _d, ...payload } = reply;
      for (const l of this.eventListeners) l(reply.event, payload as PageEvent);
      return;
    }
    const w = this.waiters.get(reply.id);
    if (!w) return;
    this.waiters.delete(reply.id);
    if (reply.ok) w.resolve(reply.result);
    else w.reject(new PincerError(reply.error.code, reply.error.message, reply.error.rpc));
  }
}
