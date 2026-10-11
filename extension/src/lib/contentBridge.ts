/**
 * Content-script side of the bridge: relays page frames to the background over a
 * `chrome.runtime` port and background replies back to the page.
 *
 * The port is created lazily and recreated if the service worker was torn down
 * (MV3 workers are killed when idle; the port then fires `onDisconnect`).
 */
import { isPageReply, isPageRequest, type PageReply, type PageRequest } from '../protocol';
import type { MessageEventLike, WindowLike } from './pageTransport';

export interface PortLike {
  postMessage(msg: unknown): void;
  onMessage(listener: (msg: unknown) => void): void;
  onDisconnect(listener: () => void): void;
  disconnect(): void;
}

export interface ContentBridgeDeps {
  win: WindowLike;
  connect(): PortLike;
}

export interface ContentBridge {
  /** Number of frames forwarded to the background (for tests). */
  readonly forwarded: number;
  dispose(): void;
}

export function createContentBridge(deps: ContentBridgeDeps): ContentBridge {
  const { win } = deps;
  const origin = win.location.origin;
  let port: PortLike | null = null;
  let forwarded = 0;

  const toPage = (reply: PageReply): void => {
    win.postMessage(reply, origin);
  };

  const getPort = (): PortLike => {
    if (port) return port;
    const p = deps.connect();
    p.onMessage((msg) => {
      if (isPageReply(msg)) toPage(msg);
    });
    p.onDisconnect(() => {
      if (port === p) port = null;
    });
    port = p;
    return p;
  };

  const onMessage = (ev: MessageEventLike): void => {
    if (ev.source !== win) return;
    if (ev.origin !== origin) return;
    if (!isPageRequest(ev.data)) return;
    const req: PageRequest = ev.data;
    try {
      getPort().postMessage(req);
      forwarded++;
    } catch (e) {
      port = null;
      toPage({
        channel: 'pincer1',
        dir: 'ext→page',
        id: req.id,
        ok: false,
        error: { code: 'INTERNAL', message: `extension unavailable: ${(e as Error).message}` },
      });
    }
  };

  win.addEventListener('message', onMessage);

  return {
    get forwarded() {
      return forwarded;
    },
    dispose() {
      win.removeEventListener('message', onMessage);
      port?.disconnect();
      port = null;
    },
  };
}
