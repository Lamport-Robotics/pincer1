/** EVM over the extension socket in the shapes the extension sends (PROTOCOL §3.1): 0x hex RLP and 0x hex messages. */
import { describe, it, expect, afterAll, beforeAll } from "vitest";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import WebSocket from "ws";
import { parseTransaction, recoverMessageAddress, type Hex } from "viem";
import { ApiServer } from "../src/api/server.js";
import { evmHarness, tx1559, FRIEND, ETH } from "./evm_fixtures.js";

const ORIGIN = "https://app.uniswap.org";
let api: ApiServer; let h: ReturnType<typeof evmHarness>; let port: number;

beforeAll(async () => {
  h = evmHarness({ rpcState: { balance: {} } });
  api = new ApiServer({ signer: h.signer, controller: h.sim, robot: h.robot, agent: null, getOwner: () => h.sim.publicKey, dataDir: mkdtempSync(join(tmpdir(), "pincer-")), bind: "127.0.0.1", port: 0 });
  await api.listen();
  port = (api.address as any).port;
});
afterAll(() => api.close());

async function connected() {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  const queue: any[] = []; const waiters: ((m: any) => void)[] = [];
  ws.on("message", (r) => { const m = JSON.parse(r.toString()); const w = waiters.shift(); w ? w(m) : queue.push(m); });
  const next = () => new Promise<any>((res) => (queue.length ? res(queue.shift()) : waiters.push(res)));
  const send = (o: unknown) => ws.send(JSON.stringify(o));
  await new Promise<void>((r) => ws.on("open", () => r()));
  send({ t: "pair", code: await api.newPairingCode() });
  const paired = await next();
  send({ t: "connect", token: paired.token, origin: ORIGIN });
  expect((await next()).t).toBe("connected");
  return { ws, next, send };
}
const hex = (b: Uint8Array) => ("0x" + Buffer.from(b).toString("hex")) as Hex;

describe("extension protocol: evm", () => {
  it("signs an unsigned EIP-1559 transaction sent as 0x hex, and still reads base64", async () => {
    const c = await connected();
    const unsigned = tx1559({ to: FRIEND, value: ETH / 100n });
    for (const [id, tx] of [["e1", hex(unsigned)], ["e2", Buffer.from(unsigned).toString("base64")]]) {
      c.send({ t: "signTransaction", id, chain: "evm", chainId: 1, tx, origin: ORIGIN });
      const ex = await c.next();
      expect(ex).toMatchObject({ t: "explain", id, chain: "evm" });
      expect(ex.summary).toMatch(/0\.01 ETH/);
      const signed = await c.next();
      expect(signed).toMatchObject({ t: "signed", id, chain: "evm" });
      const back = parseTransaction(signed.tx);
      expect(back.to?.toLowerCase()).toBe(FRIEND.toLowerCase());
      expect(back.value).toBe(ETH / 100n);
    }
    c.ws.close();
  });

  it("personal_sign: a 0x hex message is signed as those bytes", async () => {
    const c = await connected();
    const message = "0x68656c6c6f" as Hex; // "hello"
    c.send({ t: "signMessage", id: "m1", chain: "evm", message, origin: ORIGIN });
    const signed = await c.next();
    expect(signed).toMatchObject({ t: "signed", id: "m1", chain: "evm" });
    const who = await recoverMessageAddress({ message: { raw: message }, signature: signed.signature });
    expect(who.toLowerCase()).toBe(h.owner.toLowerCase());
    c.ws.close();
  });
});
