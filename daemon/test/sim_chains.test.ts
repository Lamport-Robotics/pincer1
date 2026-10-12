/** The software twin's EVM policy checks (PROTOCOL §1 multi-chain), driven directly like a compromised daemon would. */
import { describe, it, expect } from "vitest";
import { hashTypedData, keccak256, maxUint256, recoverAddress, type Hex } from "viem";
import { SimController } from "../src/controller/sim.js";
import { parseEvmTx } from "../src/chains/evm/parse.js";
import type { EvmClaims } from "../src/controller/types.js";
import * as E from "./evm_fixtures.js";

function sim(o: { dailyLimitWei?: bigint } = {}) {
  const s = new SimController({ seed: E.seed, autoTouch: true, dailyLimitWei: o.dailyLimitWei });
  const claimsFor = (bytes: Uint8Array, routers: string[] = [E.ROUTER]): EvmClaims => {
    const p = parseEvmTx(bytes, { owner: s.evmAddress }, { routers: new Set(routers.map((r) => r.toLowerCase())) });
    return { chain_id: p.chainId, to: p.to ?? "", value_wei: p.value.toString(), token_out: p.tokenOut.map((t) => ({ token: t.token ?? "", amount: t.amount.toString(), to: t.dest ?? "" })), max_fee_wei: p.maxFeeWei.toString(), routers, dangerous: p.dangers.map((d) => d.kind as EvmClaims["dangerous"][number]) };
  };
  return { s, claimsFor };
}

describe("sim controller: EVM", () => {
  it("hello lists every address; the three are distinct key families", async () => {
    const { s } = sim();
    const h = await s.hello();
    expect(h.addresses).toEqual({ solana: s.publicKey, evm: s.evmAddress, bitcoin: s.bitcoinAddress });
    expect(h.addresses!.evm).toMatch(/^0x[0-9a-fA-F]{40}$/); expect(h.addresses!.bitcoin).toMatch(/^bc1q/);
    expect(h.policy.daily_limit_wei).toBe((10n ** 18n).toString()); expect(h.policy.daily_limit_sats).toBe("5000000");
  });
  it("signs when the claims match, with a 65-byte signature that recovers to its address", async () => {
    const { s, claimsFor } = sim();
    const bytes = E.tx1559({ to: E.STRANGER, value: E.ETH / 10n });
    const r = await s.sign("evm_tx", bytes, claimsFor(bytes));
    expect(r.ok).toBe(true); if (!r.ok) return;
    expect(r.sig).toHaveLength(65); expect([0, 1]).toContain(r.sig[64]);
    const digest = keccak256(("0x" + Buffer.from(bytes).toString("hex")) as Hex);
    const who = await recoverAddress({ hash: digest, signature: { r: ("0x" + Buffer.from(r.sig.subarray(0, 32)).toString("hex")) as Hex, s: ("0x" + Buffer.from(r.sig.subarray(32, 64)).toString("hex")) as Hex, yParity: r.sig[64] } });
    expect(who.toLowerCase()).toBe(s.evmAddress.toLowerCase());
    const leg = E.txLegacy({ to: E.STRANGER, value: 1n });
    const rl = await s.sign("evm_tx", leg, claimsFor(leg));
    expect(rl.ok && [27, 28].includes(rl.sig[64])).toBe(true);
  });
  it("rejects understated value, wrong chain id, wrong destination, understated fee", async () => {
    const { s, claimsFor } = sim();
    const bytes = E.tx1559({ to: E.STRANGER, value: E.ETH / 10n });
    const c = claimsFor(bytes);
    expect(await s.sign("evm_tx", bytes, { ...c, value_wei: "1" })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await s.sign("evm_tx", bytes, { ...c, chain_id: 8453 })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await s.sign("evm_tx", bytes, { ...c, to: E.FRIEND })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await s.sign("evm_tx", bytes, { ...c, max_fee_wei: "1" })).toEqual({ ok: false, reason: "claims_mismatch" });
  });
  it("refuses undeclared approvals, understated token amounts and undeclared unknown contracts", async () => {
    const { s, claimsFor } = sim();
    const ap = E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.EOA_SPENDER), gas: 60_000n });
    expect(claimsFor(ap).dangerous).toEqual(["approve_unlimited"]);
    expect(await s.sign("evm_tx", ap, { ...claimsFor(ap), dangerous: [] })).toEqual({ ok: false, reason: "policy" });
    expect(await s.sign("evm_tx", ap, { ...claimsFor(ap), dangerous: ["approve"] })).toEqual({ ok: false, reason: "policy" });
    const tr = E.tx1559({ to: E.USDC, data: E.erc20Transfer(E.STRANGER, 25_000_000n), gas: 60_000n });
    expect(await s.sign("evm_tx", tr, { ...claimsFor(tr), token_out: [{ token: E.USDC, amount: "1", to: E.STRANGER }] })).toEqual({ ok: false, reason: "claims_mismatch" });
    // same amount, wrong recipient
    expect(await s.sign("evm_tx", tr, { ...claimsFor(tr), token_out: claimsFor(tr).token_out.map((t) => ({ ...t, to: E.FRIEND })) })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await s.sign("evm_tx", tr, { ...claimsFor(tr), token_out: [] })).toEqual({ ok: false, reason: "claims_mismatch" });
    const unk = E.tx1559({ to: E.CONTRACT, data: "0xdeadbeef" as Hex, gas: 100_000n });
    expect(await s.sign("evm_tx", unk, { ...claimsFor(unk), dangerous: [] })).toEqual({ ok: false, reason: "policy" });
    // the same call to a built-in router is not "unknown"
    const viaRouter = E.tx1559({ to: E.ROUTER, data: "0xdeadbeef" as Hex, gas: 100_000n });
    expect((await s.sign("evm_tx", viaRouter, claimsFor(viaRouter))).ok).toBe(true);
  });
  it("host router list is ignored", async () => {
    const { s, claimsFor } = sim();
    // the host "blesses" an arbitrary contract as a router and omits the unknown_contract danger
    const call = E.tx1559({ to: E.CONTRACT, data: "0xdeadbeef" as Hex, gas: 100_000n });
    const lying = claimsFor(call, [E.CONTRACT]);
    expect(lying.dangerous).not.toContain("unknown_contract");
    expect(await s.sign("evm_tx", call, lying)).toEqual({ ok: false, reason: "policy" });
    // the owner adds it on the controller with a long touch; from then on it is a router
    expect(await s.addRouter(E.CONTRACT)).toBe(true);
    expect((await s.sign("evm_tx", call, lying)).ok).toBe(true);
    expect(await s.addRouter("not-an-address")).toBe(false);
  });
  it("daily limit is counted per chain in wei and lifted by a long touch", async () => {
    const { s, claimsFor } = sim({ dailyLimitWei: E.ETH / 2n });
    const bytes = E.tx1559({ to: E.STRANGER, value: E.ETH / 10n * 3n });
    expect((await s.sign("evm_tx", bytes, claimsFor(bytes))).ok).toBe(true);
    expect(await s.sign("evm_tx", bytes, claimsFor(bytes))).toEqual({ ok: false, reason: "over_daily_limit" });
    expect((await s.hello()).policy.spent_today_wei).toBe((E.ETH / 10n * 3n).toString());
    expect(await s.elevate(5n * E.ETH)).toBe(true);
    expect((await s.sign("evm_tx", bytes, claimsFor(bytes))).ok).toBe(true);
    expect(await s.setChainPolicy("evm", 10n * E.ETH)).toBe(true);
    expect((await s.hello()).policy.daily_limit_wei).toBe((10n * E.ETH).toString());
  });
  it("EIP-712: re-hashes the typed data and refuses a digest that does not match; EIP-191 refuses disguised transactions", async () => {
    const { s } = sim();
    const td = { domain: { name: "X", chainId: 1 }, types: { Order: [{ name: "amount", type: "uint256" }] }, primaryType: "Order", message: { amount: "5" } };
    const digest = hashTypedData({ domain: { name: "X", chainId: 1 }, types: td.types, primaryType: "Order", message: { amount: 5n } });
    const good = await s.sign("evm_msg", new Uint8Array(Buffer.from(digest.slice(2), "hex")), { kind: "eip712", typed_data: JSON.stringify(td), digest });
    expect(good.ok).toBe(true);
    const forged = new Uint8Array(32).fill(1);
    expect(await s.sign("evm_msg", forged, { kind: "eip712", typed_data: JSON.stringify(td), digest: ("0x" + "01".repeat(32)) as Hex })).toEqual({ ok: false, reason: "claims_mismatch" });
    expect(await s.sign("evm_msg", forged, { kind: "eip712", typed_data: "{}", digest })).toEqual({ ok: false, reason: "policy" });
    expect(await s.sign("evm_msg", E.tx1559({ to: E.STRANGER, value: maxUint256 / 2n }), { kind: "eip191" })).toEqual({ ok: false, reason: "policy" });
    expect((await s.sign("evm_msg", new TextEncoder().encode("hello"), { kind: "eip191" })).ok).toBe(true);
  });
  it("lock and duress apply to every chain", async () => {
    const { s, claimsFor } = sim();
    const bytes = E.tx1559({ to: E.STRANGER, value: 1n });
    await s.lock();
    expect(await s.sign("evm_tx", bytes, claimsFor(bytes))).toEqual({ ok: false, reason: "locked" });
    await s.unlock();
    const real = s.evmAddress;
    await s.duress();
    expect(s.evmAddress).not.toBe(real); expect(s.addresses.bitcoin).not.toBe(new SimController({ seed: E.seed }).bitcoinAddress);
  });
});
