/** The extension-facing WebSocket contract (PROTOCOL.md §3) against a live ApiServer on a random port. */
import { describe, it, expect, afterAll, beforeAll } from "vitest";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Connection } from "@solana/web3.js";
import WebSocket from "ws";
import { ApiServer } from "../src/api/server.js";
import { SimController } from "../src/controller/sim.js";
import { MockRobot } from "../src/presence/client.js";
import { Signer } from "../src/sign.js";
import { Intel } from "../src/risk/intel.js";
import type { Config } from "../src/config.js";
import * as F from "./fixtures.js";

const seed = new Uint8Array(32).fill(7);
let api: ApiServer; let sim: SimController; let robot: MockRobot; let port: number;

beforeAll(async () => {
  sim = new SimController({ seed, autoTouch: true });
  robot = new MockRobot();
  const conn = new Connection("http://127.0.0.1:1");
  const intel = new Intel(conn, 50);
  intel.seed([{ address: F.stranger.toBase58(), ageDays: 300, txCount: 200 }]);
  const cfg = { PINCER_MODE: "dev", addressBook: {}, originAllowlist: ["https://pump.fun"], dailyLimitLamports: 2_000_000_000n } as unknown as Config;
  const signer = new Signer(cfg, sim, robot, conn, () => sim.publicKey, intel);
  api = new ApiServer({ signer, controller: sim, robot, agent: null, getOwner: () => sim.publicKey, dataDir: mkdtempSync(join(tmpdir(), "pincer-")), bind: "127.0.0.1", port: 0 });
  await api.listen();
  port = (api.address as any).port;
});
afterAll(() => api.close());

function client() {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  const queue: any[] = []; const waiters: ((m: any) => void)[] = [];
  ws.on("message", (r) => { const m = JSON.parse(r.toString()); const w = waiters.shift(); w ? w(m) : queue.push(m); });
  const next = () => new Promise<any>((res) => (queue.length ? res(queue.shift()) : waiters.push(res)));
  const send = (o: unknown) => ws.send(JSON.stringify(o));
  return { ws, next, send, open: new Promise<void>((r) => ws.on("open", () => r())) };
}

describe("extension protocol", () => {
  it("pair → connect → signTransaction (explain then signed); unauthorized before pairing", async () => {
    const c = client(); await c.open;
    c.send({ t: "signTransaction", id: "x", tx: "" });
    expect((await c.next()).error).toBe("unauthorized");
    const code = await api.newPairingCode();
    expect(robot.spoken.at(-1)).toMatch(/Pairing code/);
    c.send({ t: "pair", code: "000000" === code ? "111111" : "000000" });
    expect((await c.next()).error).toBe("bad_code");
    c.send({ t: "pair", code: await api.newPairingCode() });
    const paired = await c.next();
    expect(paired.t).toBe("paired");
    expect(paired.pubkey).toBe(sim.publicKey);

    const c2 = client(); await c2.open;
    c2.send({ t: "connect", token: paired.token, origin: "https://pump.fun" });
    expect((await c2.next()).t).toBe("connected");
    const tx = Buffer.from(F.legacy([F.transfer(F.stranger, 0.25)])).toString("base64");
    c2.send({ t: "signTransaction", id: "t1", tx });
    const ex = await c2.next();
    expect(ex).toMatchObject({ t: "explain", id: "t1", risk: "low" });
    expect(ex.summary).toMatch(/Sends 0.25 SOL/);
    const signed = await c2.next();
    expect(signed).toMatchObject({ t: "signed", id: "t1" });
    expect(Buffer.from(signed.tx, "base64").length).toBeGreaterThan(100);
    c.ws.close(); c2.ws.close();
  });

  it("denies a blocked transaction with the reason", async () => {
    const code = await api.newPairingCode();
    const c = client(); await c.open;
    c.send({ t: "pair", code });
    await c.next();
    c.send({ t: "signTransaction", id: "t2", tx: Buffer.from(F.legacy([F.setOwnerAuthority(F.stranger)])).toString("base64"), origin: "https://evil.site" });
    const ex = await c.next();
    expect(ex.risk).toBe("block");
    const d = await c.next();
    expect(d).toMatchObject({ t: "denied", id: "t2", reason: "blocked" });
    c.ws.close();
  });

  it("GET /status and POST /explain work over http", async () => {
    const s = await fetch(`http://127.0.0.1:${port}/status`).then((r) => r.json());
    expect(s.pubkey).toBe(sim.publicKey);
    expect(s.controller.se050).toBe(false);
    const e = await fetch(`http://127.0.0.1:${port}/explain`, { method: "POST", body: JSON.stringify({ tx: Buffer.from(F.legacy([F.transfer(F.stranger, 1)])).toString("base64") }) }).then((r) => r.json());
    expect(e.parsed.lamportsOut).toBe("1000000000");
    expect(e.risk).toBe("low");
  });
});
