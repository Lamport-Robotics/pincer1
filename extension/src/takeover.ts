/**
 * Opt-in `window.ethereum` takeover. Registered dynamically (chrome.scripting, MAIN world,
 * document_start) by the background ONLY while the popup toggle is on, so by default this file
 * never runs and `window.ethereum` is left to whatever other wallet the user has.
 *
 * Both this and inpage.js run before any page script, in either order: if inpage.js already
 * built the provider we define it now, otherwise we leave a flag inpage.js picks up.
 */
import { defineWindowEthereum } from './evm/provider';

const w = window as unknown as {
  __pincer1TakeoverEthereum?: boolean;
  pincer1?: { ethereum?: import('./evm/provider').Pincer1EthereumProvider };
};

if (w.pincer1?.ethereum) defineWindowEthereum(window, w.pincer1.ethereum);
else w.__pincer1TakeoverEthereum = true;
