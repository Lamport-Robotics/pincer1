/** Optional features: settings store, room lock + two-person gates, offline payments, inheritance timer, alerts. */
import { describe, it, expect } from "vitest";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Connection, PublicKey, SystemProgram, Transaction } from "@solana/web3.js";
import { SimController } from "../src/controller/sim.js";
import { MockRobot } from "../src/presence/client.js";
import { Signer } from "../src/sign.js";
import { Intel } from "../src/risk/intel.js";
import { SettingsStore } from "../src/settings.js";
import { Offline } from "../src/offline/index.js";
import { Inheritance } from "../src/inheritance/index.js";
import { Alerts } from "../src/alerts/index.js";
import { claimsFor, parseMessage } from "../src/tx/parse.js";
import type { Config } from "../src/config.js";
import * as F from "./fixtures.js";

const seed = new Uint8Array(32).fill(7);
const cfg = { PINCER_MODE: "dev", addressBook: {}, originAllowlist: [], dailyLimitLamports: 100_000_000_000n } as unknown as Config;
const tmp = () => mkdtempSync(join(tmpdir(), "pincer-feat-"));

function harness(settings: Partial<ReturnType<SettingsStore["get"]>> = {}) {
  const sim = new SimController({ seed, autoTouch: true, dailyLimitLamports: 100_000_000_000n });
  const robot = new MockRobot();
  const conn = new Connection("http://127.0.0.1:1");
  const intel = new Intel(conn, 50);
  intel.seed([{ address: F.stranger.toBase58(), ageDays: 300, txCount: 200 }, { address: F.friend.toBase58(), ageDays: 300, txCount: 200 }]);
  const store = new SettingsStore(tmp(), settings);
  const signer = new Signer(cfg, sim, robot, conn, () => sim.publicKey, intel, 300, () => store.get());
  return { sim, robot, conn, signer, store };
}

describe("settings", () => {
  it("everything optional is off by default and survives a restart", () => {
    const dir = tmp();
    const a = new SettingsStore(dir);
    expect(a.get()).toMatchObject({ roomLock: false, twoPersonSol: 0, airgap: "off", offlinePayments: false, agent: false, inheritance: { enabled: false } });
    a.update({ roomLock: true, twoPersonSol: 5, inheritance: { heir: "x" } });
    const b = new SettingsStore(dir);
    expect(b.get().roomLock).toBe(true);
    expect(b.get().twoPersonSol).toBe(5);
    expect(b.get().inheritance).toMatchObject({ heir: "x", inactivityDays: 90 });
  });
  it("rejects nonsense", () => {
    const s = new SettingsStore(tmp());
    expect(() => s.update({ airgap: "sometimes" })).toThrow();
  });
});

describe("room lock and two-person rule", () => {
  it("room lock off: signs even in a strange room", async () => {
    const { robot, signer } = harness();
    robot.set({ room_match: 0.1 });
    expect((await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 0.1)]), source: "extension" })).ok).toBe(true);
  });
  it("room lock on: refuses outside the enrolled room, signs inside", async () => {
    const { robot, signer } = harness({ roomLock: true });
    robot.set({ room_match: 0.3 });
    const r = await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 0.1)]), source: "extension" });
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toBe("not_home");
    expect(robot.spoken.at(-1)).toMatch(/not my room/);
    robot.set({ room_match: 0.95 });
    expect((await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 0.1)]), source: "extension" })).ok).toBe(true);
  });
  it("two-person rule: big transfers need two bodies and two enrolled faces", async () => {
    const { robot, signer } = harness({ twoPersonSol: 5 });
    robot.set({ persons: 1, owners: ["can"] });
    expect((await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 1)]), source: "extension" })).ok).toBe(true);
    const big = await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 6)]), source: "extension" });
    expect(big.ok).toBe(false);
    if (!big.ok) expect(big.reason).toBe("two_person_required");
    robot.set({ persons: 2, owners: ["can", "spouse"] });
    expect((await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 6)]), source: "extension" })).ok).toBe(true);
  });
});

describe("offline payments", () => {
  it("signs now, keeps the tx, renders a QR, broadcasts later", async () => {
    const { signer, conn } = harness({ offlinePayments: true });
    let sent: Buffer | null = null;
    (conn as any).sendRawTransaction = async (raw: Buffer) => { sent = Buffer.from(raw); return "sig111"; };
    const off = new Offline(tmp(), signer, conn);
    const r = await off.sign(F.legacy([F.transfer(F.friend, 0.2)]));
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.qrSvg).toMatch(/^<svg/);
    expect(off.pending()).toHaveLength(1);
    const b = await off.broadcast(r.id);
    expect(b).toEqual({ ok: true, signature: "sig111" });
    expect(sent!.toString("base64")).toBe(r.signedTx);
    expect(off.pending()).toHaveLength(0);
  });
});

describe("inheritance (dead-man's switch on the controller)", () => {
  function nonceTx(sim: SimController, heir: PublicKey) {
    const owner = new PublicKey(sim.publicKey);
    const nonceAcc = F.friend; // pretend nonce account
    const adv = SystemProgram.nonceAdvance({ noncePubkey: nonceAcc, authorizedPubkey: owner });
    const tx = new Transaction({ feePayer: owner, nonceInfo: { nonce: F.BLOCKHASH, nonceInstruction: adv } }).add(SystemProgram.transfer({ fromPubkey: owner, toPubkey: heir, lamports: 1_000_000_000 }));
    return new Uint8Array(tx.compileMessage().serialize());
  }
  it("arms only with a durable-nonce tx and a long touch; releases only after inactivity; touches reset", async () => {
    const { sim } = harness();
    const plain = F.messageOf(F.legacy([F.transfer(F.stranger, 1)]));
    const r0 = await sim.inheritanceSet(plain, claimsFor(parseMessage(plain, { owner: sim.publicKey })), { inactivityDays: 30, warnDays: [7, 1], heir: F.stranger.toBase58() });
    expect(r0).toMatchObject({ ok: false, reason: "not_durable_nonce" });
    const msg = nonceTx(sim, F.stranger);
    const r1 = await sim.inheritanceSet(msg, claimsFor(parseMessage(msg, { owner: sim.publicKey })), { inactivityDays: 30, warnDays: [7, 1], heir: F.stranger.toBase58() });
    expect(r1.ok).toBe(true);
    expect((await sim.hello()).inheritance).toMatchObject({ armed: true, inactivity_days: 30 });
    expect(await sim.inheritanceRelease()).toEqual({ ok: false, reason: "not_expired" });
    const warnings: number[] = [];
    sim.onEvent((e) => { if (e.ev === "inheritance_warning") warnings.push(e.days_left!); });
    sim.tick(20 * 86_400);
    sim.touch(); // owner is alive: timer resets
    sim.tick(25 * 86_400);
    expect((await sim.inheritanceStatus()).days_since_touch).toBeCloseTo(25, 1);
    expect(warnings).toEqual([7]); // 5 days left: the 7-day warning fired, the 1-day one not yet
    expect(await sim.inheritanceRelease()).toEqual({ ok: false, reason: "not_expired" });
    sim.tick(6 * 86_400);
    expect(warnings).toEqual([7, 1]);
    const rel = await sim.inheritanceRelease();
    expect(rel.ok).toBe(true);
    if (rel.ok) expect(rel.tx.length).toBe(65 + msg.length);
  });
  it("lock and duress do not cancel it; the daemon-side tick broadcasts when expired", async () => {
    const { sim, robot, conn } = harness();
    const msg = nonceTx(sim, F.stranger);
    await sim.inheritanceSet(msg, claimsFor(parseMessage(msg, { owner: sim.publicKey })), { inactivityDays: 10, warnDays: [], heir: F.stranger.toBase58() });
    await sim.lock(); await sim.duress();
    expect((await sim.inheritanceStatus()).armed).toBe(true);
    (conn as any).sendRawTransaction = async () => "inheritsig";
    const inh = new Inheritance(sim, robot, conn, () => sim.realPublicKey);
    expect(await inh.tick()).toBeNull();
    sim.tick(11 * 86_400);
    expect(await inh.tick()).toEqual({ ok: true, signature: "inheritsig" });
    expect(robot.networkLog.at(-1)).toMatchObject({ on: true, reason: "broadcast" });
    expect(robot.spoken.at(-1)).toMatch(/passed the wallet to your heir/);
  });
});

describe("alerts", () => {
  function fakeFetch(prices: { sol?: number; btc?: number }) {
    return (async (url: string) => ({ ok: true, json: async () => (String(url).includes("coingecko") ? { bitcoin: { usd: prices.btc } } : { data: { So11111111111111111111111111111111111111112: { price: prices.sol } } }) })) as unknown as typeof fetch;
  }
  it("wakes the owner in person when it can, phones otherwise", async () => {
    const robot = new MockRobot();
    const conn = new Connection("http://127.0.0.1:1");
    (conn as any).getBalance = async () => 10e9;
    const sent: string[] = [];
    const notifier = { ntfyTopic: "pincer-test" };
    const a = new Alerts(conn, robot, () => F.owner.publicKey.toBase58(), notifier, () => true, () => false, fakeFetch({ sol: 100 }));
    const origFetch = (a as any).fetchImpl;
    (a as any).fetchImpl = (async (url: string, init?: any) => { if (String(url).includes("ntfy.sh")) { sent.push(init.body); return { ok: true }; } return origFetch(url); }) as any;
    a.add({ asset: "SOL", dropPct: 10, reference: "set", wakeAtNight: false, enabled: true });
    expect((await a.tick()).tripped).toHaveLength(0); // sets the reference at 100
    (a as any).fetchImpl = (async (url: string, init?: any) => { if (String(url).includes("ntfy.sh")) { sent.push(init.body); return { ok: true }; } return fakeFetch({ sol: 85 })(url); }) as any;
    const t = await a.tick(Date.now() + 1000);
    expect(t.tripped).toHaveLength(1);
    expect(robot.wakes).toHaveLength(1);
    expect(robot.wakes[0].lines[1]).toBe("Dude. It's in the red.");
    expect(robot.wakes[0].lines[2]).toMatch(/down 15 percent/);
    expect(sent).toHaveLength(0); // acked by the robot, no phone
    // owner away: phone alarm, no wake
    robot.set({ owner: false, room_match: 0.2 });
    a.reset(a.list()[0].id, 100);
    const t2 = await a.tick(Date.now() + 2000);
    expect(t2.tripped).toHaveLength(1);
    expect(robot.wakes).toHaveLength(1);
    expect(sent[0]).toMatch(/Pincer1: SOL down 15.0%.*you are not home/);
  });
  it("respects quiet hours unless the rule says wake at night, with a cooldown", async () => {
    const robot = new MockRobot();
    const conn = new Connection("http://127.0.0.1:1");
    const a = new Alerts(conn, robot, () => F.owner.publicKey.toBase58(), {}, () => true, () => true, fakeFetch({ btc: 50_000 }));
    a.add({ asset: "BTC", dropPct: 5, reference: "set", wakeAtNight: false, enabled: true });
    a.add({ asset: "BTC", dropPct: 5, reference: "set", wakeAtNight: true, enabled: true });
    await a.tick();
    (a as any).fetchImpl = fakeFetch({ btc: 40_000 });
    const t = await a.tick(Date.now() + 1000);
    expect(t.tripped).toHaveLength(2);
    expect(robot.wakes).toHaveLength(1); // only the wake-at-night rule woke the owner
    expect((await a.tick(Date.now() + 2000)).tripped).toHaveLength(0); // cooldown
  });
});
