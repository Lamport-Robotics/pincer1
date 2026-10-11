/**
 * Isolated-world content script (document_start). Bridges window.postMessage frames from the
 * MAIN-world wallet to the background service worker over a chrome.runtime port.
 */
import { createContentBridge, type PortLike } from './lib/contentBridge';
import { PORT_PAGE } from './protocol';

function chromePort(): PortLike {
  const port = chrome.runtime.connect({ name: PORT_PAGE });
  return {
    postMessage: (m) => port.postMessage(m),
    onMessage: (l) => port.onMessage.addListener(l),
    onDisconnect: (l) => port.onDisconnect.addListener(l),
    disconnect: () => port.disconnect(),
  };
}

if (window.location.origin && window.location.origin !== 'null') {
  createContentBridge({ win: window, connect: chromePort });
}
