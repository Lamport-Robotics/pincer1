/** Firmware update flow against the sim: the same checks as firmware/src/update.c, then the long touch. */
import { describe, it, expect } from "vitest";
import { SimController } from "../src/controller/sim.js";
import { checkImage, flashFirmware, confirmFirmware, padImage, inspectImage, sha256Hex } from "../src/firmware.js";

const seed = new Uint8Array(32).fill(9);

/** A plausible RP2350 image head: vector table, a block with IMAGE_TYPE (exe, secure, rp2350), SIGNATURE, VERSION, LAST. */
function makeImage(size: number, opts: { exeSecure?: boolean; signed?: boolean; version?: [number, number] } = {}): Uint8Array {
  const img = new Uint8Array(size);
  for (let i = 0; i < size; i++) img[i] = (i * 7 + 3) & 0xff;
  const dv = new DataView(img.buffer);
  let p = 0x100;
  const put = (v: number) => { dv.setUint32(p, v >>> 0, true); p += 4; };
  put(0xffffded3);
  const flags = opts.exeSecure === false ? 0x2 : 0x1 | (0x2 << 4) | (0x1 << 12);
  put(0x42 | (1 << 8) | (flags << 16));
  if (opts.signed !== false) { put(0x09 | (3 << 8)); put(0x11111111); put(0x22222222); }
  if (opts.version) { put(0x48 | (2 << 8)); put((opts.version[0] << 16) | opts.version[1]); }
  put(0xff | (1 << 8)); put(0); put(0xab123579);
  return img;
}

describe("image checks", () => {
  it("accepts a signed secure RP2350 executable and reads the embedded version", () => {
    const r = checkImage(makeImage(8192, { version: [0, 5] }));
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.info.embedded).toBe("0.5");
  });
  it("rejects unsigned, data-only and blockless images", () => {
    expect(checkImage(makeImage(4096, { signed: false }))).toMatchObject({ ok: false, reason: expect.stringContaining("signed") });
    expect(checkImage(makeImage(4096, { exeSecure: false }))).toMatchObject({ ok: false });
    expect(inspectImage(new Uint8Array(4096).fill(0x5a))).toBeNull();
  });
  it("pads to flash pages", () => {
    const p = padImage(new Uint8Array(1000).fill(1));
    expect(p.length).toBe(1024);
    expect(p[1023]).toBe(0xff);
    expect(padImage(new Uint8Array(512)).length).toBe(512);
  });
});

describe("flash flow on the sim", () => {
  it("stages, the owner's long touch installs, the next daemon commits", async () => {
    const sim = new SimController({ seed, autoTouch: true });
    const img = makeImage(10_000, { version: [0, 5] }); // not page aligned: padded on the way
    const sent: number[] = [];
    const r = await flashFirmware(sim, img, "0.5.0", { onProgress: (s) => sent.push(s) });
    expect(r).toMatchObject({ ok: true, version: "0.5.0", bytes: 10_240 });
    expect(sent.at(-1)).toBe(10_240);
    const h = await sim.hello();
    expect(h.fw).toBe("0.5.0");
    expect(h.update).toEqual({ partition: 1, staged: false, trial: true, secure_boot: false });
    expect(await confirmFirmware(sim)).toEqual({ ok: true, was_trial: true, bought: false });
    expect((await sim.hello()).update?.trial).toBe(false);
  });
  it("refuses without the touch and leaves nothing staged after a bad hash or image", async () => {
    const sim = new SimController({ seed, autoTouch: false });
    const img = makeImage(4096);
    const r = await flashFirmware(sim, img, "0.5.0", { touchTtlMs: 300 });
    expect(r).toMatchObject({ ok: false, step: "apply", reason: "timeout" });
    expect((await sim.hello()).fw).toBe("sim-0.2.0");
    expect(await flashFirmware(sim, makeImage(4096, { signed: false }), "0.5.0")).toMatchObject({ ok: false, step: "check" });
    expect(await flashFirmware(sim, img, "banana")).toMatchObject({ ok: false, step: "check" });
    expect(await flashFirmware(sim, makeImage(4096, { version: [0, 4] }), "0.5.0")).toMatchObject({ ok: false, step: "check", reason: expect.stringContaining("0.4.x") });
    // the controller's own checks: wrong hash announced
    expect(await sim.updateBegin(4096, "00".repeat(32), "0.5.0")).toMatchObject({ ok: true });
    expect(await sim.updateChunk(0, img.subarray(0, 1024))).toEqual({ ok: true });
    expect(await sim.updateChunk(0, img.subarray(0, 1024))).toEqual({ ok: false, reason: "offset" });
    for (let off = 1024; off < 4096; off += 1024) await sim.updateChunk(off, img.subarray(off, off + 1024));
    expect(await sim.updateEnd()).toEqual({ ok: false, reason: "hash" });
    expect((await sim.hello()).update?.staged).toBe(false);
    expect(await sim.updateApply()).toEqual({ ok: false, reason: "state" });
  });
  it("locked or duress controllers do not install", async () => {
    const sim = new SimController({ seed, autoTouch: true });
    const img = makeImage(4096);
    await sim.lock();
    expect(await flashFirmware(sim, img, "0.5.0")).toMatchObject({ ok: false, step: "apply", reason: "locked" });
    expect(sha256Hex(new Uint8Array(0))).toBe("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  });
});
