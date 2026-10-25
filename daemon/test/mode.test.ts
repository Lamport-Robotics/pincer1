/** settings.mode / settings.brain gating, GET /status addresses, multi-chain signTransaction over the extension socket. */
import { describe, it, expect, afterAll, beforeAll } from "vitest";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import WebSocket from "ws";
import { parseTransaction, type Hex } from "viem";
import { ApiServer } from "../src/api/server.js";
import { SettingsStore, SettingsError } from "../src/settings.js";
import { SwapService } from "../src/swap/index.js";
import * as E from "./evm_fixtures.js";

const tmp = () => mkdtempSync(join(tmpdir(), "pincer-mode-"));

describe("settings: mode and brain", () => {
  it("defaults to online + lamport; refuses a cloud brain in offline mode with a clear error; persists", () => {
    const dir = tmp();
    const s = new SettingsStore(dir);
    expect(s.get()).toMatchObject({ mode: "online", brain: "lamport", unlimitedApproveToRouters: false, swapMaxSlippageBps: 100 });
    s.update({ brain: "claude" });
    expect(s.get().brain).toBe("claude");
    expect(() => s.update({ mode: "offline" })).toThrow(SettingsError);
    try { s.update({ mode: "offline" }); } catch (e) { expect((e as SettingsError).code).toBe("offline_mode"); expect((e as Error).message).toMatch(/claude brain needs the network/); }
    s.update({ mode: "offline", brain: "lamport" });
    expect(() => s.update({ brain: "deepseek" })).toThrow(/offline mode/);
    expect(() => s.update({ swapMaxSlippageBps: 500 })).toThrow(); // never above 1%
    expect(new SettingsStore(dir).get()).toMatchObject({ mode: "offline", brain: "lamport" });
  });
});

describe("api: multi-chain status, settings and signTransaction", () => {
  let api: ApiServer; let port: number; let h: ReturnType<typeof E.evmHarness>; let store: SettingsStore; let robotSettings: Record<string, unknown>[];
  beforeAll(async () => {
    h = E.evmHarness({ rpcState: { balance: { [E.STRANGER.toLowerCase()]: E.ETH }, nonce: { [E.STRANGER.toLowerCase()]: 5 } } });
    store = new SettingsStore(tmp());
    const swap = new SwapService({ signer: h.signer, settings: () => store.get(), evm: h.evm, fetchImpl: (async () => { throw new Error("network must not be touched"); }) as unknown as typeof fetch });
    api = new ApiServer({ signer: h.signer, controller: h.sim, robot: h.robot, agent: null, settings: store, swap, getOwner: () => h.sim.publicKey, dataDir: tmp(), bind: "127.0.0.1", port: 0 });
    await api.listen();
    port = (api.address as any).port;
    robotSettings = h.robot.settingsLog;
  });
  afterAll(() => api.close());
  const post = (path: string, body: unknown) => fetch(`http://127.0.0.1:${port}${path}`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) }).then(async (r) => ({ status: r.status, body: await r.json() }));

  it("GET /status lists every address, the chains with adapters, mode and brain", async () => {
    const s = await fetch(`http://127.0.0.1:${port}/status`).then((r) => r.json());
    expect(s.addresses).toEqual({ solana: h.sim.publicKey, evm: h.sim.evmAddress, bitcoin: h.sim.bitcoinAddress });
    expect(s.chains).toEqual(["solana", "evm"]); // no Bitcoin adapter registered in this harness
    expect(s.mode).toBe("online"); expect(s.brain).toBe("lamport");
    expect(s.controller.policy.daily_limit_wei).toBeDefined();
  });
  it("offline mode: cloud brain refused (400), swaps refused (403); settings are pushed to the robot", async () => {
    expect((await post("/settings", { mode: "offline" })).status).toBe(200);
    expect(robotSettings.at(-1)).toMatchObject({ mode: "offline", brain: "lamport" });
    const r = await post("/settings", { brain: "openai" });
    expect(r.status).toBe(400); expect(r.body.error).toBe("offline_mode"); expect(r.body.detail).toMatch(/Switch mode to online first/);
    const q = await post("/swap/quote", { from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: "SOL" }, amount: "1" });
    expect(q.status).toBe(403); expect(q.body.error).toBe("offline_mode");
    expect((await post("/swap/execute", { quoteId: "x" })).status).toBe(403);
    expect((await post("/settings", { mode: "online", brain: "claude" })).status).toBe(200);
    expect(robotSettings.at(-1)).toMatchObject({ mode: "online", brain: "claude" });
    const s = await fetch(`http://127.0.0.1:${port}/status`).then((r) => r.json());
    expect(s.brain).toBe("claude");
    // online but with an unknown quote: a 400 with the swap error, not a crash
    const ex = await post("/swap/execute", { quoteId: "nope" });
    expect(ex.status).toBe(400); expect(ex.body.error).toBe("unknown_quote");
  });
  it("POST /explain with chain evm", async () => {
    const tx = "0x" + Buffer.from(E.tx1559({ to: E.STRANGER, value: E.ETH / 4n })).toString("hex");
    const e = await post("/explain", { chain: "evm", tx: Buffer.from(tx.slice(2), "hex").toString("base64"), chainId: 1 });
    expect(e.status).toBe(200); expect(e.body.chain).toBe("evm"); expect(e.body.parsed.nativeOut).toBe((E.ETH / 4n).toString()); expect(e.body.risk).toBe("low");
    expect(e.body.summary).toMatch(/^On Ethereum: Sends 0.25 ETH/);
  });
  it("extension socket: signTransaction with chain evm returns explain then a signed RLP; chainId mismatch is denied; bitcoin needs a psbt", async () => {
    const code = await api.newPairingCode();
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    const queue: any[] = []; const waiters: ((m: any) => void)[] = [];
    ws.on("message", (r) => { const m = JSON.parse(r.toString()); const w = waiters.shift(); w ? w(m) : queue.push(m); });
    const next = () => new Promise<any>((res) => (queue.length ? res(queue.shift()) : waiters.push(res)));
    await new Promise<void>((r) => ws.on("open", () => r()));
    ws.send(JSON.stringify({ t: "pair", code }));
    const paired = await next();
    expect(paired.t).toBe("paired");
    // the extension learns every chain's address from this reply; without them connectChain fails later
    expect(paired.addresses).toEqual({ solana: h.sim.publicKey, evm: h.sim.evmAddress, bitcoin: h.sim.bitcoinAddress });
    const tx = Buffer.from(E.tx1559({ to: E.STRANGER, value: E.ETH / 4n })).toString("base64");
    ws.send(JSON.stringify({ t: "signTransaction", id: "e1", chain: "evm", chainId: 1, tx, origin: "https://app.uniswap.org" }));
    const ex = await next();
    expect(ex).toMatchObject({ t: "explain", id: "e1", chain: "evm", risk: "low" });
    const signed = await next();
    expect(signed).toMatchObject({ t: "signed", id: "e1", chain: "evm" });
    expect(parseTransaction(signed.tx as Hex).r).toBeDefined();
    ws.send(JSON.stringify({ t: "signTransaction", id: "e2", chain: "evm", chainId: 8453, tx, origin: "https://app.uniswap.org" }));
    expect((await next()).risk).toBe("block");
    expect(await next()).toMatchObject({ t: "denied", id: "e2", reason: "blocked" });
    ws.send(JSON.stringify({ t: "signTransaction", id: "b1", chain: "bitcoin", tx }));
    expect(await next()).toMatchObject({ t: "denied", id: "b1", reason: "parse_error" });
    ws.send(JSON.stringify({ t: "signTransaction", id: "x1", chain: "dogecoin", tx }));
    expect(await next()).toMatchObject({ t: "denied", id: "x1", reason: "bad_chain" });
    ws.send(JSON.stringify({ t: "signMessage", id: "m1", chain: "evm", message: Buffer.from("hello").toString("base64") }));
    const sm = await next();
    expect(sm).toMatchObject({ t: "signed", id: "m1", chain: "evm" }); expect(sm.signature).toMatch(/^0x[0-9a-f]{130}$/);
    ws.close();
  });
});
