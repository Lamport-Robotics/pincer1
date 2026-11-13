/**
 * End-to-end through the daemon with the sim controller and a mock robot: the same path a real request
 * takes, minus hardware and RPC (intel is seeded, lookup tables are not needed for legacy txs).
 */
import { describe, it, expect, beforeEach } from "vitest";
import { Connection, PublicKey, Transaction, TransactionInstruction, VersionedTransaction } from "@solana/web3.js";
import nacl from "tweetnacl";
import { SimController } from "../src/controller/sim.js";
import { MockRobot } from "../src/presence/client.js";
import { Signer } from "../src/sign.js";
import { Intel } from "../src/risk/intel.js";
import { claimsFor, parseMessage, touchesPrivacyRings, PRIVACY_RINGS_PROGRAMS } from "../src/tx/parse.js";
import type { Config } from "../src/config.js";
import * as F from "./fixtures.js";

const seed = new Uint8Array(32).fill(7); // same seed as fixtures.owner
const cfg = { PINCER_MODE: "dev", addressBook: { [F.friend.toBase58()]: "Ahmet" }, originAllowlist: ["https://pump.fun"], dailyLimitLamports: 2_000_000_000n } as unknown as Config;

function harness(opts: { autoTouch?: boolean; dailyLimitLamports?: bigint; ttlMs?: number } = {}) {
  const sim = new SimController({ seed, autoTouch: opts.autoTouch ?? true, dailyLimitLamports: opts.dailyLimitLamports ?? 2_000_000_000n });
  const robot = new MockRobot();
  const conn = new Connection("http://127.0.0.1:1"); // never reached: intel is seeded and timeouts are short
  const intel = new Intel(conn, 50);
  intel.seed([{ address: F.stranger.toBase58(), ageDays: 300, txCount: 200 }, { address: F.friend.toBase58(), ageDays: 300, txCount: 200 }]);
  const signer = new Signer(cfg, sim, robot, conn, () => sim.publicKey, intel, opts.ttlMs ?? 60_000);
  return { sim, robot, signer };
}

describe("signing pipeline", () => {
  it("explains, asks, signs a normal transfer and returns a valid signature", async () => {
    const { sim, robot, signer } = harness();
    const txBytes = F.legacy([F.transfer(F.stranger, 0.5)]);
    const r = await signer.sign({ txBytes, origin: "https://pump.fun", source: "extension" });
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    const tx = VersionedTransaction.deserialize(r.signedTx);
    const sig = tx.signatures[0];
    expect(nacl.sign.detached.verify(tx.message.serialize(), sig, F.owner.publicKey.toBytes())).toBe(true);
    expect(robot.spoken[0]).toMatch(/^From pump.fun: Sends 0.5 SOL to .* Touch me to sign\.$/);
    expect(robot.spoken.at(-1)).toBe("Signed.");
    expect(sim.publicKey).toBe(F.owner.publicKey.toBase58());
  });

  it("blocks a drainer without asking for a touch", async () => {
    const { robot, signer } = harness();
    const r = await signer.sign({ txBytes: F.legacy([F.setOwnerAuthority(F.stranger)]), origin: "https://evil.site", source: "extension" });
    expect(r.ok).toBe(false);
    if (r.ok) return;
    expect(r.reason).toBe("blocked");
    expect(robot.spoken[0]).toMatch(/^I'm not signing this\. it hands over control/);
  });

  it("refuses when the owner is not in the room", async () => {
    const { robot, signer } = harness();
    robot.set({ owner: false });
    const r = await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 0.1)]), source: "extension" });
    expect(r.ok).toBe(false);
    if (r.ok) return;
    expect(r.reason).toBe("owner_not_present");
  });

  it("denies when nobody touches within the ttl", async () => {
    const { signer, robot } = harness({ autoTouch: false, ttlMs: 300 });
    const r = await signer.sign({ txBytes: F.legacy([F.transfer(F.stranger, 0.1)]), source: "extension" });
    expect(r.ok).toBe(false);
    if (r.ok) return;
    expect(r.reason).toBe("timeout");
    expect(robot.spoken.at(-1)).toBe("No touch. Not signed.");
  });

  it("the controller rejects claims that understate the amount", async () => {
    const { sim } = harness();
    const msg = F.messageOf(F.legacy([F.transfer(F.stranger, 5)]));
    const p = parseMessage(msg, { owner: sim.publicKey });
    const claims = { ...claimsFor(p), lamports_out: "100" }; // a compromised daemon lying
    const r = await sim.sign("tx", msg, claims);
    expect(r).toEqual({ ok: false, reason: "claims_mismatch" });
  });

  it("dest must match claims", async () => {
    const { sim } = harness();
    const msg = F.messageOf(F.legacy([F.transfer(F.stranger, 1)]));
    const p = parseMessage(msg, { owner: sim.publicKey });
    // the daemon describes a transfer to the friend while the bytes pay the stranger
    const r = await sim.sign("tx", msg, { ...claimsFor(p), dest: F.friend.toBase58() });
    expect(r).toEqual({ ok: false, reason: "claims_mismatch" });
    const seen: Record<string, unknown>[] = [];
    const ok = await sim.sign("tx", msg, claimsFor(p), 1000, (pend) => { if (pend.parsed) seen.push(pend.parsed); });
    expect(ok.ok).toBe(true);
    // the controller's own reading (what its display shows) comes back before the touch
    expect(seen[0]).toMatchObject({ lamports_out: "1000000000", dest: F.stranger.toBase58() });
  });

  it("init returns pubkeys only", async () => {
    const fresh = new SimController({ autoTouch: true, initialized: false });
    const r = await fresh.init();
    expect(r.pubkey).toBeTruthy();
    expect(JSON.stringify(r)).not.toMatch(/mnemonic|sim sim/);
    // the decoy's public addresses come back once so the owner can fund it
    expect(r.decoy_addresses?.solana).toBeTruthy(); expect(r.decoy_addresses?.solana).not.toBe(r.pubkey);
  });

  it("the controller refuses undeclared dangerous instructions and undeclared programs", async () => {
    const { sim } = harness();
    const msg = F.messageOf(F.legacy([F.approveUnlimited(F.stranger)]));
    const p = parseMessage(msg, { owner: sim.publicKey });
    const r1 = await sim.sign("tx", msg, { ...claimsFor(p), dangerous: [] });
    expect(r1).toEqual({ ok: false, reason: "policy" });
    const r2 = await sim.sign("tx", msg, { ...claimsFor(p), programs: [] });
    expect(r2).toEqual({ ok: false, reason: "claims_mismatch" });
  });

  it("enforces the daily limit on the controller and allows elevation with a long touch", async () => {
    const { sim } = harness({ dailyLimitLamports: 1_000_000_000n });
    const msg = F.messageOf(F.legacy([F.transfer(F.stranger, 1.5)]));
    const claims = claimsFor(parseMessage(msg, { owner: sim.publicKey }));
    expect(await sim.sign("tx", msg, claims)).toEqual({ ok: false, reason: "over_daily_limit" });
    expect(await sim.elevate(5_000_000_000n)).toBe(true);
    expect((await sim.sign("tx", msg, claims)).ok).toBe(true);
  });

  it("lock blocks signing; duress switches to the decoy key", async () => {
    const { sim } = harness();
    const msg = F.messageOf(F.legacy([F.transfer(F.stranger, 0.1)]));
    const claims = claimsFor(parseMessage(msg, { owner: sim.publicKey }));
    await sim.lock();
    expect(await sim.sign("tx", msg, claims)).toEqual({ ok: false, reason: "locked" });
    expect(await sim.unlock()).toBe(true);
    const real = sim.publicKey;
    await sim.duress();
    expect(sim.publicKey).not.toBe(real);
    // the message was built for the real key, so the decoy slot refuses it: the attacker gets nothing
    expect(await sim.sign("tx", msg, claims)).toEqual({ ok: false, reason: "claims_mismatch" });
  });

  it("duress rhythm: unlock looks identical, opens the decoy, the daemon cannot tell", async () => {
    const { sim } = harness();
    const events: string[] = []; sim.onEvent((e) => events.push(e.ev));
    // identical to the real PIN is refused; a different rhythm is accepted
    sim.rhythm("real"); expect(await sim.setDuressPin()).toEqual({ ok: false, reason: "pin_mismatch" });
    sim.rhythm("duress"); expect(await sim.setDuressPin()).toEqual({ ok: true });
    const real = sim.publicKey;
    await sim.lock();
    sim.rhythm("duress");
    expect(await sim.unlock()).toBe(true);               // same reply as a real unlock
    expect(events).not.toContain("pin_failed");           // no failure counted
    expect(sim.publicKey).not.toBe(real);                 // decoy slot active
    const h = await sim.hello();
    expect((h as any).duress).toBeUndefined();            // nothing on the wire says so
    expect(h.pubkey).toBe(sim.publicKey);
    expect(await sim.decoyAddresses()).toEqual({ ok: false, reason: "duress" });
    expect(await sim.setDuressPin()).toEqual({ ok: false, reason: "duress" });
    // a wrong rhythm still fails like before
    await sim.lock(); sim.rhythm("wrong");
    expect(await sim.unlock()).toBe(false); expect(events).toContain("pin_failed");
    // the real rhythm brings the real wallet back
    sim.rhythm("real"); expect(await sim.unlock()).toBe(true); expect(sim.publicKey).toBe(real);
    const d = await sim.decoyAddresses();
    expect(d.ok && d.addresses.solana !== real && /^0x/.test(d.addresses.evm!)).toBe(true);
  });

  it("signMessage refuses payloads that are really transactions", async () => {
    const { sim } = harness();
    const msg = F.messageOf(F.legacy([F.transfer(F.stranger, 0.1)]));
    expect(await sim.sign("msg", msg, { lamports_out: "0", token_out: [], dest: "", programs: [], dangerous: [] })).toEqual({ ok: false, reason: "policy" });
    const r = await sim.sign("msg", new TextEncoder().encode("Sign in to pump.fun"), { lamports_out: "0", token_out: [], dest: "", programs: [], dangerous: [] });
    expect(r.ok).toBe(true);
  });

  it("signs a version 1 transaction: read from its own layout, and the signature goes after the message", async () => {
    const { robot, signer } = harness();
    const message = F.v1Message([F.transfer(F.stranger, 0.5)], { priorityLamports: 20_000n, cuLimit: 200_000 });
    const txBytes = Uint8Array.from([...message, ...new Uint8Array(64)]); // unsigned: an empty signature slot after the message
    const r = await signer.sign({ txBytes, origin: "https://pump.fun", source: "extension" });
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.parsed.version).toBe(1);
    expect(r.parsed.feeLamports).toBe(25_000n);
    expect(robot.spoken[0]).toMatch(/^From pump.fun: Sends 0.5 SOL to .* Touch me to sign\.$/);
    expect(r.signedTx).toHaveLength(txBytes.length);
    expect(Buffer.from(r.signedTx.subarray(0, message.length)).equals(Buffer.from(message))).toBe(true);
    expect(nacl.sign.detached.verify(message, r.signedTx.subarray(message.length), F.owner.publicKey.toBytes())).toBe(true);
    // the same bytes offered as a "message" are refused: a transaction is never signed blind
    expect(await signer.signMessage(message, "https://pump.fun")).toEqual({ ok: false, reason: "blocked" });
  });

  it("signMessage signs only what can be read: never a version 1 transaction, a raw hash, or a spaceless string", async () => {
    const { sim, robot, signer } = harness();
    const none = { lamports_out: "0", token_out: [], dest: "", programs: [], dangerous: [] };
    // a version 1 transaction message starts 0x81; the legacy/v0 parser does not read it, so it is not "a transaction" to looksLikeMessage
    const v1 = new Uint8Array(200).fill(0x61); v1[0] = 0x81; v1[9] = 0x20;
    const digest = new Uint8Array(32).map((_, i) => (i * 29 + 3) & 255);
    const hex = new TextEncoder().encode("ab".repeat(40));
    for (const bytes of [v1, digest, hex]) {
      expect(await sim.sign("msg", bytes, none)).toEqual({ ok: false, reason: "policy" }); // the controller, on its own
      robot.spoken.length = 0;
      expect(await signer.signMessage(bytes, "https://drainer.example")).toEqual({ ok: false, reason: "blocked" });
      expect(robot.spoken).toHaveLength(1);
      expect(robot.spoken[0]).toMatch(/^I'm not signing this\. drainer\.example sent a message I can't show you in full/);
    }
    // a raw 32-byte hash is refused for an EVM personal_sign too
    expect(await sim.sign("evm_msg", digest, {})).toEqual({ ok: false, reason: "policy" });
    // a multi-line sign-in text is still signed
    robot.spoken.length = 0;
    const ok = await signer.signMessage(new TextEncoder().encode("Sign in to pump.fun\nnonce: 1234"), "https://pump.fun");
    expect(ok.ok).toBe(true);
  });

  it("refuses a transaction that calls the Privacy Rings pool or registry, even declared as an unknown program", async () => {
    const { sim } = harness();
    for (const id of PRIVACY_RINGS_PROGRAMS) {
      const tx = new Transaction({ feePayer: F.owner.publicKey, recentBlockhash: PublicKey.default.toBase58() })
        .add(new TransactionInstruction({ programId: new PublicKey(id), keys: [{ pubkey: F.owner.publicKey, isSigner: true, isWritable: true }], data: Buffer.from([15]) }));
      const bytes = tx.serializeMessage();
      expect(touchesPrivacyRings(bytes)).toBe(true);
      expect(await sim.sign("tx", bytes, { lamports_out: "0", fee_lamports: "5000", token_out: [], dest: "", programs: [id], dangerous: ["unknown_program"] })).toEqual({ ok: false, reason: "policy" });
    }
  });

  it("signMessage never signs the Privacy Rings key-derivation message, and says who asked", async () => {
    const { sim, robot, signer } = harness();
    const none = { lamports_out: "0", token_out: [], dest: "", programs: [], dangerous: [] };
    const bare = new TextEncoder().encode("TSPP/derive/v1");
    const enveloped = new Uint8Array([0xff, ...new TextEncoder().encode("solana offchain"), 0, 0, 14, 0, ...bare]);
    // the controller refuses on its own, whatever the daemon does
    expect(await sim.sign("msg", bare, none)).toEqual({ ok: false, reason: "policy" });
    expect(await sim.sign("msg", enveloped, none)).toEqual({ ok: false, reason: "policy" });
    // the daemon blocks it before asking for a touch and names the site
    const r = await signer.signMessage(enveloped, "https://drainer.example");
    expect(r).toEqual({ ok: false, reason: "blocked" });
    expect(robot.spoken).toHaveLength(1);
    expect(robot.spoken[0]).toMatch(/^I'm not signing this\. drainer\.example asked for the signature that unlocks your private balance keys/);
    expect(robot.spoken[0]).not.toMatch(/Touch me/);
    // an ordinary message is still asked about and signed
    const ok = await signer.signMessage(new TextEncoder().encode("Sign in to pump.fun"), "https://pump.fun");
    expect(ok.ok).toBe(true);
  });

  it("an allowance lets agent trades through without a touch, within its limits", async () => {
    const { sim } = harness({ autoTouch: false });
    // grant: long touch simulated by calling touch(true) after the request is pending
    const grant = sim.allowance({ lamports: "1000000000", programs: ["11111111111111111111111111111111"], until_unix: Math.floor(Date.now() / 1000) + 3600, max_per_tx_lamports: "300000000" });
    await new Promise((r) => setImmediate(r));
    sim.touch(true);
    expect(await grant).toBe(true);
    const small = F.messageOf(F.legacy([F.transfer(F.stranger, 0.2)]));
    const c = claimsFor(parseMessage(small, { owner: sim.publicKey }));
    expect((await sim.sign("tx", small, c)).ok).toBe(true); // no touch needed
    const big = F.messageOf(F.legacy([F.transfer(F.stranger, 0.5)]));
    const pending = sim.sign("tx", big, claimsFor(parseMessage(big, { owner: sim.publicKey })), 200);
    expect((await pending).ok).toBe(false); // over max_per_tx → needs a touch → times out
    expect((await sim.hello()).allowance?.spent).toBe("200000000");
  });
});
