/** Bitcoin adapter: PSBT build / parse / explain / risk against a fake Esplora, signed by the sim, finalised by the daemon. */
import { describe, it, expect } from "vitest";
import * as bitcoin from "bitcoinjs-lib";
import { secp256k1 } from "@noble/curves/secp256k1";
import { Connection } from "@solana/web3.js";
import { Esplora } from "../src/chains/bitcoin/esplora.js";
import { buildPsbt, parsePsbt, finalisePsbt, estimateVsize } from "../src/chains/bitcoin/psbt.js";
import { explainBtc, assessBtc } from "../src/chains/bitcoin/explain.js";
import { BitcoinAdapter } from "../src/chains/bitcoin/index.js";
import { SimController } from "../src/controller/sim.js";
import { MockRobot } from "../src/presence/client.js";
import { Signer } from "../src/sign.js";
import { Intel } from "../src/risk/intel.js";
import { cfg, seed } from "./evm_fixtures.js";
import { fakeEsplora } from "./btc_fixtures.js";

const DEST = "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq";
const OTHER = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";

const utxo = (value: number, n = 1, confirmed = true) => ({ txid: n.toString(16).padStart(64, "0"), vout: 0, value, confirmed });

function harness(o: Parameters<typeof fakeEsplora>[0] & { autoTouch?: boolean; dailyLimitSats?: bigint; addressBook?: Record<string, string> } = {}) {
  const sim = new SimController({ seed, autoTouch: o.autoTouch ?? true, dailyLimitSats: o.dailyLimitSats });
  const owner = sim.bitcoinAddress;
  const fe = fakeEsplora({ ...o, utxos: o.utxos ?? { [owner]: [utxo(200_000, 1), utxo(50_000, 2), utxo(800, 3)] } });
  const btc = new BitcoinAdapter({ esplora: fe.esplora, network: "mainnet" });
  const robot = new MockRobot();
  const conn = new Connection("http://127.0.0.1:1");
  const signer = new Signer({ ...cfg, addressBook: o.addressBook ?? {} } as any, sim, robot, conn, () => sim.publicKey, new Intel(conn, 50), 500);
  signer.addresses = () => sim.addresses;
  signer.register(btc);
  return { sim, owner, btc, robot, signer, sent: fe.sent, esplora: fe.esplora };
}

describe("PSBT build", () => {
  it("largest-first coin selection with change, fee from the clamped oracle", async () => {
    const { btc, owner } = harness({ fee: { "1": 900, "3": 700 }, maxFeeRate: 50 }); // the oracle lies; the cap holds
    const fee = await btc.opts.esplora.feeRate(3);
    expect(fee).toEqual({ rate: 50, clamped: true, source: "oracle" });
    const r = await btc.buildPayment(owner, DEST, 150_000n);
    const p = parsePsbt(r.psbt, owner, "mainnet");
    expect(p.ownerInputs.map((i) => i.value)).toEqual([200_000]);
    expect(p.satsOut).toBe(150_000n); expect(p.dest).toBe(DEST);
    expect(p.changeSats).toBe(200_000n - 150_000n - r.feeSats);
    expect(r.feeSats).toBe(BigInt(Math.ceil(estimateVsize(1, [{ kind: "p2wpkh" }, { kind: "p2wpkh" }]) * 50)));
    expect(p.rbf).toBe(true); expect(p.sighashAll).toBe(true);
  });
  it("folds sub-dust change into the fee, refuses insufficient funds, falls back when the oracle is down", async () => {
    const utxos = [utxo(150_000 + 10_000, 1)];
    // 141 vB with change × 8 = 1,128 sats; 160,000 − 158,500 − 1,128 = 372 < dust → no change output, the remainder (1,500) is the fee
    const r = buildPsbt({ network: "mainnet", owner: harness().owner, dest: DEST, amountSats: 158_500n, utxos, feeRate: 8 });
    expect(r.changeSats).toBe(0n); expect(r.psbt.txOutputs).toHaveLength(1); expect(r.feeSats).toBe(1_500n);
    expect(() => buildPsbt({ network: "mainnet", owner: harness().owner, dest: DEST, amountSats: 10_000_000n, utxos, feeRate: 8 })).toThrow(/insufficient funds/);
    const down = new Esplora("https://down.test", { fetchImpl: (async () => { throw new Error("ECONNREFUSED"); }) as unknown as typeof fetch, timeoutMs: 100 });
    expect(await down.feeRate()).toEqual({ rate: 10, clamped: false, source: "fallback" });
  });
});

describe("PSBT explain and scam filter", () => {
  const ctx = (o: Partial<Parameters<typeof assessBtc>[1]> = {}) => ({ intel: {}, tokenMeta: {}, addressBook: {}, ownerBalance: 10_000_000n, owner: "", originAllowlist: [], dailyLimit: 5_000_000n, seenPrograms: new Set<string>(), routers: new Set<string>(), unlimitedApproveToRouters: false, ...o });
  it("sends 0.0015 BTC to bc1q…, fee 2,100 sats (1.4%)", async () => {
    const { btc, owner } = harness({ fee: { "3": 1 } });
    const r = await btc.buildPayment(owner, DEST, 150_000n);
    const p = parsePsbt(r.psbt, owner, "mainnet");
    (p as any).feeSats = 2_100n; // pin the fee so the sentence is exact
    const ex = explainBtc(p, ctx());
    expect(ex.spoken).toBe("Sends 0.0015 BTC to bc1qar0s…5mdq, fee 2,100 sats (1.4%).");
    expect(ex.lines).toContain("note: can be replaced with a higher fee until it confirms");
    expect(assessBtc(p, ctx()).level).toBe("low");
  });
  it("fee above 5% is high; dust inputs and reused destinations are medium", async () => {
    const { owner } = harness();
    const r = buildPsbt({ network: "mainnet", owner, dest: DEST, amountSats: 20_000n, utxos: [utxo(100_000, 1), utxo(800, 9)], feeRate: 20 });
    const p = parsePsbt(new Uint8Array(r.psbt.toBuffer()), owner, "mainnet");
    const v = assessBtc(p, ctx());
    expect(v.level).toBe("high"); expect(v.reasons[0]).toMatch(/fee is \d+% of the amount/);
    const dusty = buildPsbt({ network: "mainnet", owner, dest: DEST, amountSats: 100_500n, utxos: [utxo(100_000, 1), utxo(900, 9)], feeRate: 1 });
    const pd = parsePsbt(new Uint8Array(dusty.psbt.toBuffer()), owner, "mainnet");
    const vd = assessBtc(pd, ctx({ intel: { [DEST]: { address: DEST, exists: true, isContract: false, ageDays: null, txCount: 120, txCountIsLowerBound: false, balance: 0n, fetchedAt: 0 } } }));
    expect(vd.level).toBe("medium");
    expect(vd.reasons.join(" ")).toMatch(/dust-sized coin/); expect(vd.reasons.join(" ")).toMatch(/used 120 times/);
  });
  it("unknown extra outputs, non-ALL sighash and hidden input values are blocked", async () => {
    const { owner } = harness();
    const r = buildPsbt({ network: "mainnet", owner, dest: DEST, amountSats: 50_000n, utxos: [utxo(200_000, 1)], feeRate: 5 });
    r.psbt.addOutput({ address: OTHER, value: 1_000 });
    const p = parsePsbt(new Uint8Array(r.psbt.toBuffer()), owner, "mainnet");
    expect(p.dangers.map((d) => d.kind)).toContain("unknown_change");
    const v = assessBtc(p, ctx()); expect(v.level).toBe("block"); expect(v.reasons[0]).toMatch(/pays 2 different addresses/);
    const s = buildPsbt({ network: "mainnet", owner, dest: DEST, amountSats: 50_000n, utxos: [utxo(200_000, 1)], feeRate: 5 });
    s.psbt.updateInput(0, { sighashType: bitcoin.Transaction.SIGHASH_NONE });
    expect(assessBtc(parsePsbt(new Uint8Array(s.psbt.toBuffer()), owner, "mainnet"), ctx()).reasons[0]).toMatch(/sighash is not ALL/);
    const hidden = new bitcoin.Psbt({ network: bitcoin.networks.bitcoin });
    hidden.addInput({ hash: "11".repeat(32), index: 0 });
    hidden.addOutput({ address: DEST, value: 1000 });
    const ph = parsePsbt(new Uint8Array(hidden.toBuffer()), owner, "mainnet");
    expect(ph.inputsWithoutValue).toBe(1); expect(assessBtc(ph, ctx()).level).toBe("block");
  });
});

describe("Bitcoin pipeline through the sim controller", () => {
  it("builds, explains, signs on the controller, verifies and finalises, broadcasts", async () => {
    const h = harness({ fee: { "3": 10 }, stats: { [DEST]: { tx_count: 2, funded: 0, spent: 0 } } });
    const built = await h.btc.buildPayment(h.owner, DEST, 150_000n);
    const r = await h.signer.signChain({ chain: "bitcoin", bytes: built.psbt, source: "api" });
    expect(r.ok).toBe(true); if (!r.ok) return;
    const tx = bitcoin.Transaction.fromBuffer(Buffer.from(r.signedTx));
    expect(tx.ins).toHaveLength(1); expect(tx.ins[0].witness).toHaveLength(2);
    expect(tx.outs.map((o) => o.value)).toEqual([150_000, 200_000 - 150_000 - Number(built.feeSats)]);
    // the witness signature verifies against the sim's bitcoin key over the BIP-143 digest
    const sighash = tx.hashForWitnessV0(0, bitcoin.payments.p2pkh({ pubkey: Buffer.from(h.sim.bitcoinPubkey) }).output!, 200_000, bitcoin.Transaction.SIGHASH_ALL);
    const der = tx.ins[0].witness[0].subarray(0, -1);
    expect(secp256k1.verify(secp256k1.Signature.fromDER(der).toCompactRawBytes(), sighash, h.sim.bitcoinPubkey)).toBe(true);
    expect(h.robot.spoken[0]).toMatch(/^Sends 0.0015 BTC to bc1qar0s…5mdq, an address used 2 times before, fee [\d,]+ sats \(\d+(\.\d)?%\)\. Touch me to sign\.$/);
    const txid = await h.signer.broadcast("bitcoin", r.signedTx);
    expect(txid).toBe(tx.getId()); expect(h.sent).toHaveLength(1);
  });
  it("the controller refuses claims that understate the amount or hide a high fee; daily limit in sats", async () => {
    const h = harness({ dailyLimitSats: 100_000n });
    const built = await h.btc.buildPayment(h.owner, DEST, 150_000n);
    const parsed = await h.btc.parse(built.psbt, { owner: h.owner });
    const claims = h.btc.claims(parsed);
    expect(await h.sim.sign("btc_psbt", built.psbt, { ...claims, sats_out: "1000" })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await h.sim.sign("btc_psbt", built.psbt, { ...claims, dest: OTHER })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await h.sim.sign("btc_psbt", built.psbt, { ...claims, network: "testnet" })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await h.sim.sign("btc_psbt", built.psbt, claims)).toEqual({ ok: false, reason: "over_daily_limit" });
    const pricey = buildPsbt({ network: "mainnet", owner: h.owner, dest: DEST, amountSats: 10_000n, utxos: [utxo(200_000, 1)], feeRate: 20 });
    const pp = parsePsbt(new Uint8Array(pricey.psbt.toBuffer()), h.owner, "mainnet");
    const pc = h.btc.claims(pp);
    expect(pc.dangerous).toContain("high_fee");
    expect(await h.sim.sign("btc_psbt", new Uint8Array(pricey.psbt.toBuffer()), { ...pc, dangerous: [] })).toEqual({ ok: false, reason: "policy" });
    const ok = await h.sim.sign("btc_psbt", new Uint8Array(pricey.psbt.toBuffer()), pc);
    expect(ok.ok).toBe(true);
    if (ok.ok) { const fin = finalisePsbt(ok.sig, "mainnet", (pk, m, s) => secp256k1.verify(s, m, pk)); expect(fin.txid).toHaveLength(64); }
  });
});
