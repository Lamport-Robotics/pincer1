/** Base64 / base58 helpers that work in the page, the service worker and Node (tests). */
import bs58 from 'bs58';

export function toBase64(bytes: Uint8Array): string {
  let bin = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    bin += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(bin);
}

export function fromBase64(s: string): Uint8Array {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function fromBase58(s: string): Uint8Array {
  return bs58.decode(s);
}

export function toBase58(bytes: Uint8Array): string {
  return bs58.encode(bytes);
}

let counter = 0;
/** Short unique request id; crypto.randomUUID is available in page, worker and Node ≥ 19. */
export function newId(prefix: string): string {
  counter = (counter + 1) % 1_000_000;
  const rand =
    typeof crypto !== 'undefined' && 'randomUUID' in crypto
      ? crypto.randomUUID().slice(0, 8)
      : Math.random().toString(36).slice(2, 10);
  return `${prefix}${counter.toString(36)}-${rand}`;
}
