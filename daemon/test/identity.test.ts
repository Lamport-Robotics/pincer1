/**
 * Machine identity end to end on anvil: the peaq stand-ins and Pincer1Units from identity/contracts, the
 * activation service in-process, the sim controller signing with its identity key. Skipped when anvil or the
 * forge artifacts are missing (run `forge build` in identity/contracts).
 */
import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { spawn, type ChildProcess } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { createPublicClient, createWalletClient, encodeFunctionData, http, parseAbi, parseEther, serializeTransaction, type Address, type Hex } from "viem";
import { privateKeyToAccount } from "viem/accounts";
import { SimController } from "../src/controller/sim.js";
import { Identity, multibaseSecp256k1, verifyProof } from "../src/identity/index.js";
import { machineRegistryAbi, pincer1UnitsAbi } from "../src/identity/peaq.js";

const ANVIL = process.env.ANVIL ?? join(homedir(), ".foundry/bin/anvil");
const OUT = fileURLToPath(new URL("../../identity/contracts/out/", import.meta.url));
const SERVICE = fileURLToPath(new URL("../../identity/service/server.mjs", import.meta.url));
const ready = existsSync(ANVIL) && existsSync(join(OUT, "Pincer1Units.sol/Pincer1Units.json"));
const KEY0 = "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"; // anvil #0: deployer
const KEY1 = "0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d"; // anvil #1: Lamport attester
const CHAIN = 31337;

const artifact = (file: string, name: string) => { const a = JSON.parse(readFileSync(join(OUT, `${file}.sol/${name}.json`), "utf8")); return { abi: a.abi, bytecode: a.bytecode.object as Hex }; };
const mockAbi = parseAbi(["function mint(address to, uint256 amount)", "function setLifecycle(address l)", "function balanceOf(address) view returns (uint256)", "function serviceEndpointsOf(uint256) view returns ((string id, string serviceType, string serviceEndpoint)[])", "function verificationMethodsOf(uint256) view returns ((string id, string methodType, address controller, string publicKeyMultibase)[])"]);

describe.skipIf(!ready)("machine identity on anvil", () => {
  let anvil: ChildProcess; let rpc: string; let port: number;
  const deployer = privateKeyToAccount(KEY0); const attester = privateKeyToAccount(KEY1);
  let pub: ReturnType<typeof createPublicClient>; let wallet: ReturnType<typeof createWalletClient>;
  let token: Address, registry: Address, subscription: Address, lifecycle: Address, units: Address;
  let service: { listen: (p: number) => Promise<{ port: number }>; close: () => Promise<void> }; let serviceUrl: string;
  let sim: SimController; let identity: Identity;
  const seed = new Uint8Array(32).fill(42);

  const deploy = async (file: string, name: string, args: unknown[] = []): Promise<Address> => {
    const a = artifact(file, name);
    const hash = await wallet.deployContract({ abi: a.abi, bytecode: a.bytecode, args, account: deployer, chain: null });
    const r = await pub.waitForTransactionReceipt({ hash });
    return r.contractAddress!;
  };
  const write = async (to: Address, data: Hex) => { const hash = await wallet.sendTransaction({ to, data, account: deployer, chain: null }); await pub.waitForTransactionReceipt({ hash }); };

  beforeAll(async () => {
    port = 8545 + Math.floor(Math.random() * 1000);
    rpc = `http://127.0.0.1:${port}`;
    anvil = spawn(ANVIL, ["--port", String(port), "--silent", "--block-time", "1"], { stdio: "ignore" });
    for (let i = 0; i < 50; i++) { try { const r = await fetch(rpc, { method: "POST", headers: { "content-type": "application/json" }, body: '{"jsonrpc":"2.0","id":1,"method":"eth_chainId","params":[]}' }); if (r.ok) break; } catch { /* not yet */ } await new Promise((r) => setTimeout(r, 200)); }
    pub = createPublicClient({ transport: http(rpc) });
    wallet = createWalletClient({ transport: http(rpc), account: deployer });
    token = await deploy("MockPeaq", "MockPeaqToken");
    registry = await deploy("MockPeaq", "MockMachineRegistry");
    subscription = await deploy("MockPeaq", "MockMachineSubscription", [token]);
    lifecycle = await deploy("MockPeaq", "MockMachineStateAndSync", [registry, subscription]);
    await write(registry, encodeFunctionData({ abi: mockAbi, functionName: "setLifecycle", args: [lifecycle] }));
    await write(subscription, encodeFunctionData({ abi: mockAbi, functionName: "setLifecycle", args: [lifecycle] }));
    units = await deploy("Pincer1Units", "Pincer1Units", [registry, attester.address]);

    sim = new SimController({ seed, autoTouch: true, autoLongTouch: true, serial: "SIM-0001" });
    // on peaq the bond token is the native balance; on anvil the mock token is separate, so the robot gets both
    await write(token, encodeFunctionData({ abi: mockAbi, functionName: "mint", args: [sim.identityAddress as Address, parseEther("1")] }));

    const { createService } = await import(SERVICE);
    service = createService({ rpcUrl: rpc, chainId: CHAIN, units, attesterKey: KEY1, fundWei: parseEther("1"), stateFile: join(mkdtempSync(join(tmpdir(), "p1svc-")), "activations.json"), allowAnySerial: true, log: () => undefined });
    const a = await service.listen(0);
    serviceUrl = `http://127.0.0.1:${a.port}`;

    identity = new Identity(sim, {
      chainId: CHAIN, rpcUrl: rpc, unitsContract: units, serviceUrl, manufacturer: attester.address, dataDir: mkdtempSync(join(tmpdir(), "p1id-")),
      deployment: { id: "agung-2026-08-28", name: "anvil", explorer: "", rpc, peaqToken: token, machineRegistry: registry, machineStateAndSync: lifecycle, machineSubscription: subscription },
      firmware: async () => "sim-0.2.0", steps: async () => 1234, heartbeatEveryMs: 60_000,
    });
  }, 60_000);
  afterAll(async () => { identity?.stop(); await service?.close(); anvil?.kill(); });

  it("reports an unactivated identity before the owner touches", async () => {
    const v = await identity.status();
    expect(v).toMatchObject({ supported: true, enabled: false, activated: false, serial: "SIM-0001", did: null });
    expect(v.address).toBe(sim.identityAddress);
  });

  it("activates: long touch, service funds and attests, bond approved, peaq machine minted, record written", async () => {
    const before = await pub.getBalance({ address: sim.identityAddress as Address });
    expect(before).toBe(0n);
    const v = await identity.activate();
    expect(v.enabled).toBe(true); expect(v.activated).toBe(true);
    expect(v.did).toMatch(/^did:peaq:\d+$/);
    const id = BigInt(v.machineId!);
    expect(await pub.readContract({ address: registry, abi: machineRegistryAbi, functionName: "ownerOf", args: [id] })).toBe(sim.identityAddress);
    expect(await pub.readContract({ address: registry, abi: machineRegistryAbi, functionName: "computeTokenId", args: ["pincer1", ("0x" + Buffer.from("SIM-0001").toString("hex")) as Hex] })).toBe(id);
    const unit = await pub.readContract({ address: units, abi: pincer1UnitsAbi, functionName: "units", args: [id] });
    expect(unit[0]).toBe("SIM-0001"); expect(unit[1]).toBe(sim.identityAddress); expect(unit[2]).toBe("0.2.0"); expect(unit[3]).toBeGreaterThan(0n); expect(unit[5]).toBe(false);
    // the bond left the robot's token balance; the funding landed on its native balance and paid the gas
    expect(await pub.readContract({ address: token, abi: mockAbi, functionName: "balanceOf", args: [sim.identityAddress as Address] })).toBe(parseEther("0.6"));
    const after = await pub.getBalance({ address: sim.identityAddress as Address });
    expect(after).toBeGreaterThan(parseEther("0.9")); expect(after).toBeLessThan(parseEther("1"));
    // the DID carries the robot's key and the firmware page
    const methods = await pub.readContract({ address: registry, abi: mockAbi, functionName: "verificationMethodsOf", args: [id] });
    expect(methods[0].controller).toBe(sim.identityAddress);
    expect(methods[0].publicKeyMultibase).toMatch(/^zQ3s/);
    const eps = await pub.readContract({ address: registry, abi: mockAbi, functionName: "serviceEndpointsOf", args: [id] });
    expect(eps.find((e) => e.id === "#firmware")?.serviceEndpoint).toContain("fw-0.2.0");
    expect(v.txs.activate && v.txs.attest && v.txs.approve).toBeTruthy();
  }, 60_000);

  it("a second activate is a no-op (resumable, never pays twice)", async () => {
    const a = await identity.status();
    const b = await identity.activate();
    expect(b.txs).toEqual(a.txs);
  }, 30_000);

  it("heartbeats without a touch, even while the wallet is locked", async () => {
    await sim.lock();
    const r = await identity.heartbeat();
    expect(r.ok).toBe(true);
    const logs = await pub.getContractEvents({ address: units, abi: pincer1UnitsAbi, eventName: "Heartbeat", fromBlock: 0n });
    expect(logs.at(-1)?.args.steps).toBe(1234n);
    expect((await identity.status()).lastHeartbeat).toBeGreaterThan(0);
    await sim.unlock();
  }, 30_000);

  it("firmware changes go to the record and the DID; unchanged is a no-op", async () => {
    expect(await identity.firmwareChanged("sim-0.2.0")).toEqual({ ok: false, reason: "unchanged" });
    const r = await identity.firmwareChanged("pincer1-fw 0.3.0 (abc)");
    expect(r.ok).toBe(true);
    const id = BigInt((await identity.status()).machineId!);
    expect((await pub.readContract({ address: units, abi: pincer1UnitsAbi, functionName: "units", args: [id] }))[2]).toBe("0.3.0");
    const eps = await pub.readContract({ address: registry, abi: mockAbi, functionName: "serviceEndpointsOf", args: [id] });
    expect(eps.find((e) => e.id === "#firmware")?.serviceEndpoint).toContain("fw-0.3.0");
  }, 30_000);

  it("the controller refuses identity transactions outside the allow list, with value, or on another chain", async () => {
    const tx = (o: { to: Address; value?: bigint; chainId?: number }) => { const h = serializeTransaction({ type: "legacy", chainId: o.chainId ?? CHAIN, nonce: 5, to: o.to, value: o.value ?? 0n, data: "0x1234", gas: 50_000n, gasPrice: 1_000n }); return new Uint8Array(Buffer.from(h.slice(2), "hex")); };
    const claims = (to: Address, chainId = CHAIN) => ({ chain_id: chainId, to, value_wei: "0", token_out: [], max_fee_wei: "50000000", routers: [], dangerous: [] as never[] });
    expect(await sim.sign("id_tx", tx({ to: units }), claims(units))).toMatchObject({ ok: true });
    expect(await sim.sign("id_tx", tx({ to: deployer.address }), claims(deployer.address))).toEqual({ ok: false, reason: "policy" });
    expect(await sim.sign("id_tx", tx({ to: units, value: 1n }), claims(units))).toEqual({ ok: false, reason: "policy" });
    expect(await sim.sign("id_tx", tx({ to: units, chainId: 1 }), claims(units, 1))).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await sim.sign("id_msg", new TextEncoder().encode("hello"), { kind: "eip191" })).toEqual({ ok: false, reason: "policy" });
    const ok = await sim.sign("id_msg", new TextEncoder().encode(`pincer1-identity:activate:SIM-0001:${sim.identityAddress.toLowerCase()}`), { kind: "eip191" });
    expect(ok.ok).toBe(true);
    if (ok.ok) expect(await verifyProof(`pincer1-identity:activate:SIM-0001:${sim.identityAddress.toLowerCase()}`, ("0x" + Buffer.from(ok.sig).toString("hex")) as Hex, "SIM-0001", sim.identityAddress as Address)).toBe(true);
  });

  it("the service refuses a bad proof and a serial already bound to another key", async () => {
    const post = (b: unknown) => fetch(serviceUrl + "/activate", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(b) });
    const other = privateKeyToAccount("0x" + "7".repeat(64) as Hex);
    const msg = `pincer1-identity:activate:SIM-0001:${other.address.toLowerCase()}`;
    const sig = await other.signMessage({ message: msg });
    const r1 = await post({ serial: "SIM-0001", machine: other.address, firmware: "0.2.0", chainId: CHAIN, message: msg, signature: sig });
    expect(r1.status).toBe(409);
    const r2 = await post({ serial: "SIM-0002", machine: other.address, firmware: "0.2.0", chainId: CHAIN, message: "pincer1-identity:activate:SIM-0002:" + other.address.toLowerCase(), signature: sig });
    expect(r2.status).toBe(400);
    const r3 = await post({ serial: "nope", machine: other.address, firmware: "0.2.0", chainId: CHAIN, message: msg, signature: sig });
    expect(r3.status).toBe(400);
    const h = await (await fetch(serviceUrl + "/health")).json();
    expect(h).toMatchObject({ ok: true, chainId: CHAIN, activated: 1 });
  });

  it("encodes a secp256k1 key as publicKeyMultibase", () => {
    const h = sim.identityAddress; expect(h).toMatch(/^0x[0-9a-fA-F]{40}$/);
    expect(multibaseSecp256k1("0x02" + "11".repeat(32))).toMatch(/^zQ3s/);
    expect(() => multibaseSecp256k1("0x1234")).toThrow();
  });
});
