/**
 * Machine identity on peaq.
 *
 * The owner turns it on once (long touch). The robot then becomes a peaq machine with its own key: it pays the
 * Entry-tier bond, calls `MachineStateAndSync.activateMachine` (which mints the machine NFT in MachineRegistry
 * and stores the DID document), and writes Lamport's countersigned record into Pincer1Units. After that it
 * reports on its own, without touches: the controller firmware version whenever it changes, and a weekly
 * heartbeat with uptime hours and steps. Nothing about the owner leaves the robot.
 *
 *   daemon ──id_msg──▶ controller   proof of key possession for the activation service
 *   daemon ──POST /activate──▶ Lamport activation service   checks the serial, funds the robot's address, signs the attestation
 *   daemon ──id_tx──▶ controller ──▶ peaq   approve(bond) · activateMachine · attest · setFirmware · heartbeat
 *
 * The controller only signs zero-value calls to the contracts the owner allowed at enable time, with a bounded
 * fee, so a compromised daemon can at most burn a little gas on the robot's own address.
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { createPublicClient, decodeEventLog, encodeFunctionData, getAddress, http, parseTransaction, recoverAddress, recoverMessageAddress, serializeTransaction, keccak256, type Address, type Hex, type PublicClient } from "viem";
import type { ControllerClient, EvmClaims, IdentityStatus, PendingHook } from "../controller/types.js";
import { log } from "../config.js";
import { semverOf } from "../firmware.js";
import { DEPLOYMENTS, MACHINE_TYPE, SUBSCRIPTION_TIER, credentialSubjectOf, machineRegistryAbi, machineStateAndSyncAbi, machineSubscriptionAbi, peaqTokenAbi, pincer1UnitsAbi, type PeaqDeployment } from "./peaq.js";

export interface IdentityOptions {
  chainId: number;
  /** Overrides the published RPC for the chain (tests point it at anvil). */
  rpcUrl?: string;
  /** Overrides the published contract addresses (tests). */
  deployment?: Partial<PeaqDeployment>;
  /** Pincer1Units on this chain. */
  unitsContract: Address;
  /** Lamport activation service, e.g. https://lamportrobotics.com/activate. */
  serviceUrl: string;
  /** Recorded as `manufacturer` in MachineRegistry (informational on peaq's side). */
  manufacturer: Address;
  dataDir: string;
  /** Current controller firmware version string (`hello.fw`). */
  firmware: () => Promise<string>;
  /** Steps walked so far, from the robot; 0 when the robot does not count. */
  steps?: () => Promise<number>;
  heartbeatEveryMs?: number;
  fetch?: typeof fetch;
}

interface State {
  serial: string;
  address: Address;
  machineId: string;
  did: string;
  chainId: number;
  activatedAt?: number;
  attestedAt?: number;
  /** Firmware version last written on chain. */
  firmware?: string;
  lastHeartbeat?: number;
  /** Powered hours accumulated across daemon runs (the heartbeat's uptime). */
  uptimeHours: number;
  txs: { activate?: Hex; attest?: Hex; firmware?: Hex; heartbeat?: Hex; approve?: Hex };
}

export interface IdentityView {
  supported: boolean;
  enabled: boolean;
  activated: boolean;
  serial: string | null;
  address: string | null;
  did: string | null;
  machineId: string | null;
  chain: { id: number; name: string; explorer: string };
  firmware: string | null;
  lastHeartbeat: number | null;
  uptimeHours: number;
  busy: string | null;
  contracts: { units: Address; machineRegistry: Address; machineStateAndSync: Address };
  txs: State["txs"];
}

const WEEK = 7 * 86_400_000;
/** Enough for an activation plus a year of weekly heartbeats at peaq's fee level; the service tops up to this. */
const PROOF_PREFIX = "pincer1-identity:activate:";
/** A release page per firmware version; the DID points at it so an explorer can show what the robot runs. */
const FIRMWARE_URL = (v: string) => `https://github.com/Lamport-Robotics/pincer1/releases/tag/fw-${v}`;
const MAKER_URL = "https://lamportrobotics.com";

export class Identity {
  readonly dep: PeaqDeployment;
  private client: PublicClient;
  private state: State | null;
  private busy: string | null = null;
  private timer: NodeJS.Timeout | null = null;
  private startedAt = Date.now();
  private fetchFn: typeof fetch;

  constructor(private controller: ControllerClient, readonly opts: IdentityOptions) {
    const base = DEPLOYMENTS[opts.chainId];
    if (!base && !opts.deployment) throw new Error(`no peaq deployment for chain ${opts.chainId}`);
    this.dep = { ...(base ?? ({} as PeaqDeployment)), ...(opts.deployment ?? {}), chainId: opts.chainId } as PeaqDeployment;
    if (opts.rpcUrl) this.dep.rpc = opts.rpcUrl;
    this.client = createPublicClient({ transport: http(this.dep.rpc, { timeout: 15_000, retryCount: 1 }) });
    this.fetchFn = opts.fetch ?? fetch;
    mkdirSync(opts.dataDir, { recursive: true });
    this.state = this.load();
  }

  private path() { return join(this.opts.dataDir, "identity.json"); }
  private load(): State | null {
    try { if (existsSync(this.path())) return JSON.parse(readFileSync(this.path(), "utf8")) as State; } catch { /* unreadable: start over */ }
    return null;
  }
  private save() { if (this.state) writeFileSync(this.path(), JSON.stringify(this.state, null, 2), { mode: 0o600 }); }

  /** The contracts the identity key may call; everything else the controller refuses. */
  private allowList(): string[] { return [this.dep.peaqToken, this.dep.machineStateAndSync, this.dep.machineRegistry, this.opts.unitsContract].map((a) => a.toLowerCase()); }

  async status(): Promise<IdentityView> {
    const h = await this.controller.hello();
    const id = h.identity ?? null;
    const s = this.state;
    return {
      supported: h.identity !== undefined, enabled: !!id?.enabled, activated: !!s?.attestedAt,
      serial: h.serial ?? s?.serial ?? null, address: id?.address ?? s?.address ?? null, did: s?.did ?? null, machineId: s?.machineId ?? null,
      chain: { id: this.dep.chainId, name: this.dep.name, explorer: this.dep.explorer },
      firmware: s?.firmware ?? null, lastHeartbeat: s?.lastHeartbeat ?? null, uptimeHours: this.uptimeHours(), busy: this.busy,
      contracts: { units: this.opts.unitsContract, machineRegistry: this.dep.machineRegistry, machineStateAndSync: this.dep.machineStateAndSync },
      txs: s?.txs ?? {},
    };
  }

  private uptimeHours() { return (this.state?.uptimeHours ?? 0) + (Date.now() - this.startedAt) / 3_600_000; }

  /**
   * The whole activation, resumable: every step checks the chain first, so a crash halfway (or a second call)
   * continues where it stopped instead of paying twice.
   */
  async activate(onPending?: PendingHook): Promise<IdentityView> {
    if (this.busy) throw new Error(`busy: ${this.busy}`);
    this.busy = "starting";
    try {
      const h = await this.controller.hello();
      if (h.identity === undefined) throw new Error("controller firmware has no machine identity");
      const serial = h.serial;
      if (!serial) throw new Error("controller reports no serial number");
      let id: IdentityStatus = h.identity ?? (await this.enable(onPending));
      if (!id.enabled || id.chain_id !== this.dep.chainId) id = await this.enable(onPending);
      const address = getAddress(id.address);
      const firmware = semverOf(await this.opts.firmware()) ?? "0.0.0";
      const machineId = await this.client.readContract({ address: this.dep.machineRegistry, abi: machineRegistryAbi, functionName: "computeTokenId", args: [MACHINE_TYPE, credentialSubjectOf(serial)] });
      if (!this.state || this.state.serial !== serial || this.state.address.toLowerCase() !== address.toLowerCase()) {
        this.state = { serial, address, machineId: machineId.toString(), did: `did:peaq:${machineId.toString()}`, chainId: this.dep.chainId, uptimeHours: 0, txs: {} };
        this.save();
      }
      const st = this.state;

      // 1. the activation service: proof of key → serial check, funding, Lamport's signature
      this.busy = "activation service";
      const message = `${PROOF_PREFIX}${serial}:${address.toLowerCase()}`;
      const proof = await this.controller.sign("id_msg", new TextEncoder().encode(message), { kind: "eip191" });
      if (!proof.ok) throw new Error(`controller refused the identity proof (${proof.reason})`);
      const res = await this.fetchFn(this.opts.serviceUrl.replace(/\/$/, "") + "/activate", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ serial, machine: address, firmware, chainId: this.dep.chainId, message, signature: toHex(proof.sig) }) });
      const body = (await res.json().catch(() => ({}))) as { error?: string; attestation?: Hex; machineId?: string; funded?: { txHash?: Hex; wei?: string } | null; units?: Address };
      if (!res.ok || !body.attestation) throw new Error(`activation service: ${body.error ?? res.status}`);
      if (body.machineId && BigInt(body.machineId) !== machineId) throw new Error("activation service computed a different machine id");
      if (body.units && body.units.toLowerCase() !== this.opts.unitsContract.toLowerCase()) throw new Error("activation service names a different Pincer1Units contract");
      const attestation = body.attestation;

      // 2. the bond: wait for the funding to land, then approve exactly the bond
      this.busy = "waiting for funds";
      const bond = await this.client.readContract({ address: this.dep.machineSubscription, abi: machineSubscriptionAbi, functionName: "requiredPeaqAmount", args: [SUBSCRIPTION_TIER.entry] });
      const minted = await this.ownerOf(machineId);
      if (!minted) {
        await this.waitForBalance(address, bond + 10n ** 16n, 180_000);
        const allowance = await this.client.readContract({ address: this.dep.peaqToken, abi: peaqTokenAbi, functionName: "allowance", args: [address, this.dep.machineSubscription] });
        if (allowance < bond) {
          this.busy = "approving the bond";
          st.txs.approve = await this.send(address, this.dep.peaqToken, encodeFunctionData({ abi: peaqTokenAbi, functionName: "approve", args: [this.dep.machineSubscription, bond] }));
          this.save();
        }
        // 3. peaq activation: NFT + DID + bond in one transaction
        this.busy = "activating on peaq";
        const doc = {
          id: "", controller: address,
          verificationMethods: [{ id: "#key-1", methodType: "EcdsaSecp256k1VerificationKey2019", controller: address, publicKeyMultibase: multibaseSecp256k1(id.pubkey) }],
          authentication: [0n],
          serviceEndpoints: this.endpoints(firmware),
        };
        st.txs.activate = await this.send(address, this.dep.machineStateAndSync, encodeFunctionData({ abi: machineStateAndSyncAbi, functionName: "activateMachine", args: [doc, MACHINE_TYPE, credentialSubjectOf(serial), this.opts.manufacturer, SUBSCRIPTION_TIER.entry] }));
        st.activatedAt = Date.now();
        this.save();
      } else if (minted.toLowerCase() !== address.toLowerCase()) {
        throw new Error(`machine ${st.did} is already owned by ${minted}`);
      } else if (!st.activatedAt) { st.activatedAt = Date.now(); this.save(); }

      // 4. Lamport's record
      const unit = await this.client.readContract({ address: this.opts.unitsContract, abi: pincer1UnitsAbi, functionName: "units", args: [machineId] });
      if (unit[3] === 0n) {
        this.busy = "writing the attestation";
        st.txs.attest = await this.send(address, this.opts.unitsContract, encodeFunctionData({ abi: pincer1UnitsAbi, functionName: "attest", args: [serial, firmware, attestation] }));
      }
      st.attestedAt = Date.now();
      st.firmware = firmware;
      st.lastHeartbeat = st.lastHeartbeat ?? Date.now();
      this.save();
      log.info("identity: active", { did: st.did, address, chain: this.dep.name });
      this.schedule();
      return this.status();
    } finally { this.busy = null; }
  }

  private async enable(onPending?: PendingHook): Promise<IdentityStatus> {
    this.busy = "waiting for the owner's long touch";
    const r = await this.controller.identityEnable({ chain_id: this.dep.chainId, allow: this.allowList() }, onPending);
    if (!r.ok) throw new Error(`identity not enabled (${r.reason})`);
    return r.identity;
  }

  /** Owner's choice to stop: the key is turned off on the controller; the peaq record stays as it is. */
  async disable(onPending?: PendingHook): Promise<boolean> {
    const ok = await this.controller.identityDisable(onPending);
    if (ok) this.stop();
    return ok;
  }

  async heartbeat(): Promise<{ ok: true; tx: Hex } | { ok: false; reason: string }> {
    const st = this.state;
    if (!st?.attestedAt) return { ok: false, reason: "not_activated" };
    if (this.busy) return { ok: false, reason: `busy: ${this.busy}` };
    this.busy = "heartbeat";
    try {
      const steps = BigInt(Math.max(0, Math.floor((await this.opts.steps?.()) ?? 0)));
      const hours = Math.min(0xffffffff, Math.floor(this.uptimeHours()));
      const tx = await this.send(st.address, this.opts.unitsContract, encodeFunctionData({ abi: pincer1UnitsAbi, functionName: "heartbeat", args: [BigInt(st.machineId), hours, steps] }));
      st.uptimeHours = this.uptimeHours(); this.startedAt = Date.now();
      st.lastHeartbeat = Date.now(); st.txs.heartbeat = tx; this.save();
      return { ok: true, tx };
    } catch (e) { return { ok: false, reason: String(e) }; } finally { this.busy = null; }
  }

  /** Called after a firmware update was committed: the record and the DID both get the new version. */
  async firmwareChanged(version: string): Promise<{ ok: true; tx: Hex } | { ok: false; reason: string }> {
    const st = this.state;
    const v = semverOf(version);
    if (!st?.attestedAt || !v) return { ok: false, reason: "not_activated" };
    if (st.firmware === v) return { ok: false, reason: "unchanged" };
    if (this.busy) return { ok: false, reason: `busy: ${this.busy}` };
    this.busy = "firmware";
    try {
      const tx = await this.send(st.address, this.opts.unitsContract, encodeFunctionData({ abi: pincer1UnitsAbi, functionName: "setFirmware", args: [BigInt(st.machineId), v] }));
      try { await this.send(st.address, this.dep.machineRegistry, encodeFunctionData({ abi: machineRegistryAbi, functionName: "setServiceEndpoints", args: [BigInt(st.machineId), this.endpoints(v)] })); } catch (e) { log.warn("identity: DID firmware endpoint not updated", String(e)); }
      st.firmware = v; st.txs.firmware = tx; this.save();
      return { ok: true, tx };
    } catch (e) { return { ok: false, reason: String(e) }; } finally { this.busy = null; }
  }

  /** Weekly heartbeat, and a catch-up on boot when the last one is older than a week or the firmware moved. */
  start() {
    if (!this.state?.attestedAt) return;
    this.schedule();
    void (async () => {
      const fw = semverOf(await this.opts.firmware().catch(() => ""));
      if (fw && fw !== this.state?.firmware) { const r = await this.firmwareChanged(fw); log.info("identity: firmware", r); }
      if (Date.now() - (this.state?.lastHeartbeat ?? 0) > (this.opts.heartbeatEveryMs ?? WEEK)) { const r = await this.heartbeat(); log.info("identity: heartbeat", r); }
    })();
  }
  stop() { if (this.timer) clearInterval(this.timer); this.timer = null; }
  private schedule() {
    if (this.timer) return;
    const every = this.opts.heartbeatEveryMs ?? WEEK;
    this.timer = setInterval(() => { if (this.state?.attestedAt && Date.now() - (this.state.lastHeartbeat ?? 0) >= every) void this.heartbeat().then((r) => log.info("identity: heartbeat", r)); }, Math.min(every, 3_600_000));
    this.timer.unref?.();
  }

  private endpoints(firmware: string) {
    return [{ id: "#firmware", serviceType: "Firmware", serviceEndpoint: FIRMWARE_URL(firmware) }, { id: "#maker", serviceType: "Manufacturer", serviceEndpoint: MAKER_URL }];
  }

  private async ownerOf(machineId: bigint): Promise<Address | null> {
    try { return await this.client.readContract({ address: this.dep.machineRegistry, abi: machineRegistryAbi, functionName: "ownerOf", args: [machineId] }); } catch { return null; }
  }

  private async waitForBalance(address: Address, wei: bigint, timeoutMs: number) {
    const until = Date.now() + timeoutMs;
    for (;;) {
      const bal = await this.client.getBalance({ address });
      if (bal >= wei) return;
      if (Date.now() > until) throw new Error(`robot address ${address} holds ${bal} wei, needs ${wei} for the bond and gas`);
      await new Promise((r) => setTimeout(r, 3000));
    }
  }

  /**
   * One legacy (EIP-155) transaction from the identity address: build, let the controller check and sign it,
   * assemble, broadcast, wait for the receipt. Reverts surface as errors with the receipt status.
   */
  private async send(from: Address, to: Address, data: Hex): Promise<Hex> {
    const [nonce, gasPrice, gasEstimate] = await Promise.all([
      this.client.getTransactionCount({ address: from, blockTag: "pending" }),
      this.client.getGasPrice(),
      this.client.estimateGas({ account: from, to, data, value: 0n }),
    ]);
    const gas = (gasEstimate * 13n) / 10n + 20_000n;
    const price = gasPrice + gasPrice / 5n + 1n;
    const tx = { type: "legacy" as const, chainId: this.dep.chainId, nonce, to, value: 0n, data, gas, gasPrice: price };
    const unsigned = serializeTransaction(tx);
    const bytes = new Uint8Array(Buffer.from(unsigned.slice(2), "hex"));
    const claims: EvmClaims = { chain_id: this.dep.chainId, to, value_wei: "0", token_out: [], max_fee_wei: (gas * price).toString(), routers: [], dangerous: [] };
    log.info("identity: signing", { to, nonce, gas: gas.toString(), gasPrice: price.toString(), maxFeeWei: (gas * price).toString() });
    const r = await this.controller.sign("id_tx", bytes, claims);
    if (!r.ok) throw new Error(`controller refused the identity transaction (${r.reason})`);
    const sig = r.sig;
    const rr = toHex(sig.subarray(0, 32)), ss = toHex(sig.subarray(32, 64));
    const yParity = sig[64] >= 27 ? sig[64] - 27 : sig[64];
    const digest = keccak256(unsigned);
    const who = await recoverAddress({ hash: digest, signature: { r: rr, s: ss, yParity } });
    if (who.toLowerCase() !== from.toLowerCase()) throw new Error("controller signature does not recover to the identity address");
    const signed = serializeTransaction(tx, { r: rr, s: ss, v: BigInt(27 + yParity), yParity });
    parseTransaction(signed); // round-trips, or throws before broadcast
    const hash = await this.client.sendRawTransaction({ serializedTransaction: signed });
    const receipt = await this.client.waitForTransactionReceipt({ hash, timeout: 180_000, pollingInterval: 1500 });
    if (receipt.status !== "success") throw new Error(`transaction ${hash} reverted`);
    log.info("identity: tx", { to, hash });
    return hash;
  }

  /** Decodes the machine id out of an activation receipt (used by tests and the status page). */
  static machineIdFromLogs(logs: { data: Hex; topics: Hex[] }[]): bigint | null {
    for (const l of logs) {
      try { const ev = decodeEventLog({ abi: machineRegistryAbi, data: l.data, topics: l.topics as [Hex, ...Hex[]] }); if (ev.eventName === "MachineMinted") return ev.args.tokenId; } catch { /* other event */ }
    }
    return null;
  }
}

/** Verifies a `pincer1-identity:activate:` proof; shared with the activation service. */
export async function verifyProof(message: string, signature: Hex, serial: string, machine: Address): Promise<boolean> {
  if (message !== `${PROOF_PREFIX}${serial}:${machine.toLowerCase()}`) return false;
  try { return (await recoverMessageAddress({ message, signature })).toLowerCase() === machine.toLowerCase(); } catch { return false; }
}

const toHex = (b: Uint8Array): Hex => ("0x" + Buffer.from(b).toString("hex")) as Hex;

/** `z` + base58btc(multicodec secp256k1-pub 0xe7 0x01 ‖ compressed key): the usual publicKeyMultibase for secp256k1. */
export function multibaseSecp256k1(compressedHex: string): string {
  const key = Buffer.from(compressedHex.replace(/^0x/, ""), "hex");
  if (key.length !== 33) throw new Error("compressed secp256k1 key expected");
  return "z" + base58(Buffer.concat([Buffer.from([0xe7, 0x01]), key]));
}
const ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
function base58(buf: Buffer): string {
  let n = BigInt("0x" + buf.toString("hex"));
  let out = "";
  while (n > 0n) { out = ALPHABET[Number(n % 58n)] + out; n /= 58n; }
  for (const b of buf) { if (b !== 0) break; out = "1" + out; }
  return out;
}
