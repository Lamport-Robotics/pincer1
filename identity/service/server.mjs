#!/usr/bin/env node
/**
 * Lamport activation service.
 *
 * A robot that wants to become a peaq machine sends one request: its serial, its identity address and a
 * signature from that key. The service checks that the serial is one we built and is not already bound to
 * another key, funds the robot's address with enough PEAQ for the bond and a year of heartbeats, and returns
 * Lamport's EIP-712 attestation that the robot then writes into Pincer1Units. It never sees the robot's key
 * and never acts on peaq on the robot's behalf.
 *
 *   node server.mjs            reads the environment (see .env.example)
 *   import { createService }   tests run it in-process
 */
import http from "node:http";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { createPublicClient, createWalletClient, formatEther, getAddress, http as viemHttp, isAddress, parseAbi, recoverMessageAddress } from "viem";
import { privateKeyToAccount } from "viem/accounts";

const unitsAbi = parseAbi(["function machineIdOf(string serial) view returns (uint256)", "function attester() view returns (address)"]);
const SERIAL = /^(P1|SIM)-\d{4}$/;
const PROOF_PREFIX = "pincer1-identity:activate:";

/**
 * @param {object} o
 * @param {string} o.rpcUrl
 * @param {number} o.chainId
 * @param {`0x${string}`} o.units            Pincer1Units address on that chain
 * @param {`0x${string}`} o.attesterKey      private key; must be Pincer1Units.attester and hold PEAQ for funding
 * @param {bigint} o.fundWei                 what a fresh robot address is topped up to (bond + gas)
 * @param {string} [o.serialsFile]           JSON { "P1-0001": {}, ... }: the serials we built; absent → allowAnySerial decides
 * @param {string} o.stateFile               JSON bindings serial → { machine, machineId, funded, at }
 * @param {boolean} [o.allowAnySerial]       dev: accept any well-formed serial
 * @param {(...a: unknown[]) => void} [o.log]
 */
export function createService(o) {
  const log = o.log ?? ((...a) => console.log(new Date().toISOString(), ...a));
  const account = privateKeyToAccount(o.attesterKey);
  const transport = viemHttp(o.rpcUrl, { timeout: 15_000 });
  const pub = createPublicClient({ transport });
  const wallet = createWalletClient({ account, transport });
  const serials = o.serialsFile && existsSync(o.serialsFile) ? JSON.parse(readFileSync(o.serialsFile, "utf8")) : null;
  const state = existsSync(o.stateFile) ? JSON.parse(readFileSync(o.stateFile, "utf8")) : {};
  const save = () => writeFileSync(o.stateFile, JSON.stringify(state, null, 2), { mode: 0o600 });
  const domain = { name: "Pincer1Units", version: "1", chainId: o.chainId, verifyingContract: o.units };
  const types = { Attestation: [{ name: "machineId", type: "uint256" }, { name: "serial", type: "string" }, { name: "machine", type: "address" }] };
  /** one request at a time per serial; funding twice because of a retry would be a gift */
  const inflight = new Set();
  const recent = new Map(); // ip → timestamps, 10/min

  const json = (res, code, body) => { res.writeHead(code, { "content-type": "application/json", "cache-control": "no-store" }); res.end(JSON.stringify(body)); };
  const readBody = (req) => new Promise((resolve, reject) => { let s = ""; req.on("data", (c) => { s += c; if (s.length > 4096) { reject(new Error("too large")); req.destroy(); } }); req.on("end", () => resolve(s)); req.on("error", reject); });

  async function activate(req, res) {
    const ip = String(req.headers["x-real-ip"] ?? req.socket.remoteAddress ?? "?");
    const now = Date.now();
    const hits = (recent.get(ip) ?? []).filter((t) => now - t < 60_000);
    if (hits.length >= 10) return json(res, 429, { error: "slow down" });
    hits.push(now); recent.set(ip, hits);
    let b;
    try { b = JSON.parse(await readBody(req)); } catch { return json(res, 400, { error: "bad json" }); }
    const serial = String(b.serial ?? "");
    if (!SERIAL.test(serial)) return json(res, 400, { error: "bad serial" });
    if (!isAddress(String(b.machine ?? ""))) return json(res, 400, { error: "bad machine address" });
    const machine = getAddress(b.machine);
    if (Number(b.chainId) !== o.chainId) return json(res, 400, { error: `wrong chain, this service is for ${o.chainId}` });
    const firmware = String(b.firmware ?? "");
    if (!/^\d+\.\d+\.\d+$/.test(firmware)) return json(res, 400, { error: "bad firmware version" });
    // proof of key possession: the message names the serial and the address, the signature comes from that address
    const message = String(b.message ?? "");
    if (message !== `${PROOF_PREFIX}${serial}:${machine.toLowerCase()}`) return json(res, 400, { error: "bad proof message" });
    let signer;
    try { signer = await recoverMessageAddress({ message, signature: b.signature }); } catch { return json(res, 400, { error: "bad proof signature" }); }
    if (signer.toLowerCase() !== machine.toLowerCase()) return json(res, 400, { error: "proof not signed by the machine" });
    // the serial must be ours, and bound to this key or to nobody yet
    if (serials && !(serial in serials)) return json(res, 404, { error: "unknown serial" });
    if (!serials && !o.allowAnySerial) return json(res, 404, { error: "unknown serial" });
    const bound = state[serial];
    if (bound && bound.machine.toLowerCase() !== machine.toLowerCase()) return json(res, 409, { error: "serial already activated by another key" });
    if (inflight.has(serial)) return json(res, 409, { error: "activation in progress" });
    inflight.add(serial);
    try {
      const machineId = await pub.readContract({ address: o.units, abi: unitsAbi, functionName: "machineIdOf", args: [serial] });
      const attestation = await account.signTypedData({ domain, types, primaryType: "Attestation", message: { machineId, serial, machine } });
      let funded = null;
      if (!bound?.funded) {
        const bal = await pub.getBalance({ address: machine });
        if (bal < o.fundWei) {
          const hash = await wallet.sendTransaction({ to: machine, value: o.fundWei - bal, chain: null });
          const r = await pub.waitForTransactionReceipt({ hash, timeout: 120_000 });
          if (r.status !== "success") throw new Error("funding transaction reverted");
          funded = { txHash: hash, wei: (o.fundWei - bal).toString() };
        }
      }
      state[serial] = { machine, machineId: machineId.toString(), funded: bound?.funded ?? funded ?? { wei: "0" }, firmware, at: bound?.at ?? new Date().toISOString() };
      save();
      log("activate", serial, machine, funded ? `funded ${formatEther(BigInt(funded.wei))}` : "already funded");
      return json(res, 200, { machineId: machineId.toString(), attestation, funded, units: o.units, chainId: o.chainId, attester: account.address });
    } catch (e) {
      log("activate failed", serial, String(e?.message ?? e));
      return json(res, 502, { error: "chain error, try again" });
    } finally { inflight.delete(serial); }
  }

  const server = http.createServer((req, res) => {
    const path = (req.url ?? "/").split("?")[0];
    if (req.method === "GET" && path === "/health") return pub.getBalance({ address: account.address }).then((bal) => json(res, 200, { ok: true, chainId: o.chainId, attester: account.address, units: o.units, balance: formatEther(bal), activated: Object.keys(state).length })).catch(() => json(res, 503, { ok: false }));
    if (req.method === "POST" && path === "/activate") return activate(req, res).catch((e) => json(res, 500, { error: String(e?.message ?? e) }));
    json(res, 404, { error: "not found" });
  });
  return {
    server, account,
    listen: (port, bind = "127.0.0.1") => new Promise((r) => server.listen(port, bind, () => r(server.address()))),
    close: () => new Promise((r) => server.close(() => r())),
  };
}

const isMain = process.argv[1] && import.meta.url === new URL(`file://${process.argv[1]}`).href;
if (isMain) {
  const need = (k) => { const v = process.env[k]; if (!v) { console.error(`missing ${k}`); process.exit(2); } return v; };
  const svc = createService({
    rpcUrl: need("RPC_URL"), chainId: Number(need("CHAIN_ID")), units: need("UNITS_CONTRACT"), attesterKey: need("ATTESTER_KEY"),
    fundWei: BigInt(process.env.FUND_WEI ?? "500000000000000000"), serialsFile: process.env.SERIALS_FILE, stateFile: process.env.STATE_FILE ?? "./activations.json",
    allowAnySerial: process.env.ALLOW_ANY_SERIAL === "true",
  });
  svc.listen(Number(process.env.PORT ?? 7430), process.env.BIND ?? "127.0.0.1").then((a) => console.log("activation service", a, "attester", svc.account.address));
}
