/**
 * Controller firmware updates (docs/PROTOCOL.md §1 "update").
 *
 * The daemon only carries bytes: the controller re-hashes what lands in flash, checks the picobin block
 * (executable, secure, RP2350, signed), and the bootrom verifies the signature against the OTP key before
 * anything runs. The checks here are the same ones, done early so a wrong file fails before the long
 * touch is asked for. Nothing here can make an unsigned image boot.
 */
import { createHash } from "node:crypto";
import type { ControllerClient, PendingHook } from "./controller/types.js";

export const FW_PAGE = 256;
export const FW_CHUNK = 1024;
const HEADER_SCAN = 4096;
const BLOCK_MARKER_START = 0xffffded3;
const ITEM_IMAGE_TYPE = 0x42;
const ITEM_SIGNATURE = 0x09;
const ITEM_VERSION = 0x48;
const ITEM_LAST = 0xff;

export interface ImageInfo {
  exe: boolean;
  secure: boolean;
  rp2350: boolean;
  signed: boolean;
  /** major.minor from the VERSION item, when the build embedded one. */
  embedded?: string;
}

/** Walks the first picobin block in the image head. `null` when there is no block at all. */
export function inspectImage(img: Uint8Array): ImageInfo | null {
  const len = Math.min(img.length, HEADER_SCAN);
  const dv = new DataView(img.buffer, img.byteOffset, len);
  let start = 0;
  for (; start + 8 <= len; start += 4) if (dv.getUint32(start, true) === BLOCK_MARKER_START) break;
  if (start + 8 > len) return null;
  const info: ImageInfo = { exe: false, secure: false, rp2350: false, signed: false };
  let p = start + 4;
  while (p + 4 <= len) {
    const item = dv.getUint32(p, true);
    const type = item & 0xff;
    if (type === ITEM_LAST) return info;
    if (type === ITEM_IMAGE_TYPE) {
      const flags = item >>> 16;
      info.exe = (flags & 0x000f) === 0x1;
      info.secure = (flags & 0x0030) === 0x2 << 4;
      info.rp2350 = (flags & 0x7000) === 0x1 << 12;
    } else if (type === ITEM_SIGNATURE) {
      info.signed = true;
    } else if (type === ITEM_VERSION && p + 8 <= len) {
      const v = dv.getUint32(p + 4, true);
      info.embedded = `${v >>> 16}.${v & 0xffff}`;
    }
    let words = type & 0x80 ? (item >>> 8) & 0xffff : (item >>> 8) & 0xff;
    if (words === 0) words = 1;
    p += words * 4;
  }
  return null; // ran off the scanned window without LAST
}

export function checkImage(img: Uint8Array): { ok: true; info: ImageInfo } | { ok: false; reason: string } {
  const info = inspectImage(img);
  if (!info) return { ok: false, reason: "no RP2350 image block in the first 4 KiB" };
  if (!info.exe || !info.secure || !info.rp2350) return { ok: false, reason: "not a secure RP2350 executable" };
  if (!info.signed) return { ok: false, reason: "image is not signed" };
  return { ok: true, info };
}

/** Flash pages are 256 bytes; the tail is padded with erased-flash bytes. */
export function padImage(img: Uint8Array): Uint8Array {
  const rem = img.length % FW_PAGE;
  if (!rem) return img;
  const out = new Uint8Array(img.length + FW_PAGE - rem).fill(0xff);
  out.set(img);
  return out;
}

export const sha256Hex = (b: Uint8Array) => createHash("sha256").update(b).digest("hex");

export type FlashOutcome = { ok: true; version: string; bytes: number } | { ok: false; step: "check" | "begin" | "chunk" | "end" | "apply"; reason: string };

/**
 * Stage `image` on the controller and ask the owner to install it. Resolves `ok` once the controller has
 * accepted the long touch: it then reboots into the new image, the serial link drops, and the daemon
 * restarts against the new firmware, which `confirmFirmware` commits.
 */
export async function flashFirmware(ctrl: ControllerClient, image: Uint8Array, version: string, opts: { onProgress?: (sent: number, total: number) => void; onPending?: PendingHook; touchTtlMs?: number } = {}): Promise<FlashOutcome> {
  const c = checkImage(image);
  if (!c.ok) return { ok: false, step: "check", reason: c.reason };
  if (!/^\d+\.\d+\.\d+$/.test(version)) return { ok: false, step: "check", reason: "version must be major.minor.patch" };
  if (c.info.embedded && !version.startsWith(c.info.embedded + ".")) return { ok: false, step: "check", reason: `image says ${c.info.embedded}.x, caller says ${version}` };
  const img = padImage(image);
  const b = await ctrl.updateBegin(img.length, sha256Hex(img), version);
  if (!b.ok) return { ok: false, step: "begin", reason: b.reason };
  const chunk = Math.min(b.chunk, FW_CHUNK);
  for (let off = 0; off < img.length; off += chunk) {
    const r = await ctrl.updateChunk(off, img.subarray(off, Math.min(off + chunk, img.length)));
    if (!r.ok) { await ctrl.updateAbort().catch(() => {}); return { ok: false, step: "chunk", reason: r.reason }; }
    opts.onProgress?.(Math.min(off + chunk, img.length), img.length);
  }
  const e = await ctrl.updateEnd();
  if (!e.ok) return { ok: false, step: "end", reason: e.reason };
  const a = await ctrl.updateApply(opts.onPending, opts.touchTtlMs);
  if (!a.ok) return { ok: false, step: "apply", reason: a.reason };
  return { ok: true, version, bytes: img.length };
}

/**
 * Called at daemon start when `hello.update.trial` is set: the new image booted and answers the protocol,
 * so the trial ends here. Without this the controller steps back to the previous image after three boots.
 */
export async function confirmFirmware(ctrl: ControllerClient) {
  const r = await ctrl.updateCommit();
  return r.ok ? { ok: true as const, was_trial: r.was_trial, bought: r.bought } : { ok: false as const, reason: r.reason };
}
