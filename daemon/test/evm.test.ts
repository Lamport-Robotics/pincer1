/** EVM adapter: parse, explain, scam filter, EIP-712, assembly, and the full pipeline through the sim controller. */
import { describe, it, expect } from "vitest";
import { getAddress, hashTypedData, keccak256, maxUint256, parseTransaction, recoverAddress, recoverTypedDataAddress, recoverMessageAddress, type Hex } from "viem";
import { parseEvmTx, looksLikeEvmTx } from "../src/chains/evm/parse.js";
import { explainEvm } from "../src/chains/evm/explain.js";
import { assessEvm } from "../src/chains/evm/risk.js";
import { EvmIntel } from "../src/chains/evm/intel.js";
import { canonicalTypedDataJson, parseTypedData, typedDataDigest, explainTypedData, assessTypedData, permitsOf } from "../src/chains/evm/message.js";
import type { ChainAddressIntel } from "../src/chains/types.js";
import * as E from "./evm_fixtures.js";

const own = E.evmHarness().owner;
const intel = (address: string, o: Partial<ChainAddressIntel>): ChainAddressIntel => ({ address, exists: true, isContract: false, ageDays: 400, txCount: 50, txCountIsLowerBound: true, balance: 10n * E.ETH, verified: null, fetchedAt: 0, ...o });
const routers = new Set([E.ROUTER.toLowerCase()]);
const parse = (bytes: Uint8Array, requestedChainId?: number) => parseEvmTx(bytes, { owner: own, requestedChainId }, { routers });
const ctx = (o: Partial<Parameters<typeof assessEvm>[1]> = {}) => ({ intel: {}, tokenMeta: {}, addressBook: {}, ownerBalance: 10n * E.ETH, owner: own, originAllowlist: [], dailyLimit: E.ETH, knownPrograms: new Set<string>(), routers, unlimitedApproveToRouters: false, ...o });

describe("EVM parse", () => {
  it("reads an EIP-1559 value transfer", () => {
    const p = parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 2n }));
    expect(p).toMatchObject({ chainId: 1, type: "eip1559", value: E.ETH / 2n, nativeOut: E.ETH / 2n, dest: getAddress(E.STRANGER), maxFeeWei: 21_000n * 20n * E.GWEI, dangers: [], programs: [] });
    expect(p.call.fn).toBe("unknown");
  });
  it("reads a legacy EIP-155 transaction and refuses signed ones", () => {
    const p = parse(E.txLegacy({ to: E.STRANGER, value: 1n, chainId: 56 }));
    expect(p.type).toBe("legacy"); expect(p.chainId).toBe(56);
    expect(looksLikeEvmTx(E.txLegacy({ to: E.STRANGER }))).toBe(true);
    expect(looksLikeEvmTx(new TextEncoder().encode("hello"))).toBe(false);
  });
  it("decodes ERC-20 transfer, approve (limited and unlimited), setApprovalForAll", () => {
    const t = parse(E.tx1559({ to: E.USDC, data: E.erc20Transfer(E.STRANGER, 25_000_000n), gas: 60_000n }));
    expect(t.tokenOut).toEqual([{ token: E.USDC, amount: 25_000_000n, dest: getAddress(E.STRANGER) }]);
    expect(t.dest).toBe(getAddress(E.STRANGER));
    const a = parse(E.tx1559({ to: E.USDC, data: E.erc20Approve(E.ROUTER, 1000n), gas: 60_000n }));
    expect(a.dangers).toEqual([{ kind: "approve", detail: "token allowance", to: E.ROUTER, amount: 1000n }]);
    const u = parse(E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.EOA_SPENDER), gas: 60_000n }));
    expect(u.dangers[0]).toMatchObject({ kind: "approve_unlimited", to: getAddress(E.EOA_SPENDER), amount: maxUint256 });
    const s = parse(E.tx1559({ to: E.CONTRACT, data: ("0xa22cb465" + "0".repeat(24) + E.EOA_SPENDER.slice(2) + "0".repeat(63) + "1") as Hex, gas: 60_000n }));
    expect(s.call.fn).toBe("setApprovalForAll"); expect(s.dangers[0].kind).toBe("set_approval_for_all");
  });
  it("calls into an allowlisted router are not 'unknown contract'; other calldata is", () => {
    const r = parse(E.tx1559({ to: E.ROUTER, data: "0x07ed2379" + "00".repeat(64) as Hex, gas: 200_000n }));
    expect(r.call.fn).toBe("router"); expect(r.dangers).toEqual([]);
    const u = parse(E.tx1559({ to: E.CONTRACT, data: "0xdeadbeef" as Hex, gas: 200_000n }));
    expect(u.dangers[0].kind).toBe("unknown_contract");
  });
});

describe("EVM explain", () => {
  it("sends 0.5 ETH to a young contract", () => {
    const p = parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 2n }));
    const ex = explainEvm(p, { intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, { isContract: true, ageDays: 2.4 }) }, tokenMeta: {}, addressBook: {}, ownerBalance: null });
    expect(ex.spoken).toBe("On Ethereum: Sends 0.5 ETH to 0x1234…1234, a contract deployed 2 days ago.");
    expect(ex.lines.at(-1)).toMatch(/^note: fee up to 0\.00042 ETH/);
  });
  it("unlimited USDC approval reads as a danger, with origin and chain", () => {
    const p = parse(E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.EOA_SPENDER), gas: 60_000n }));
    const ex = explainEvm(p, { origin: "https://evil.site", intel: {}, tokenMeta: {}, addressBook: {}, ownerBalance: null });
    expect(ex.spoken).toMatch(/^From evil.site, on Ethereum: DANGER: gives 0x0000…dead unlimited permission to spend your USDC\.$/i);
  });
  it("token transfer uses symbol and decimals, router call says swap", () => {
    const t = explainEvm(parse(E.tx1559({ to: E.USDC, data: E.erc20Transfer(E.FRIEND, 25_000_000n), gas: 60_000n })), { intel: {}, tokenMeta: {}, addressBook: { [E.FRIEND]: "Ahmet" }, ownerBalance: null });
    expect(t.spoken).toBe("On Ethereum: Sends 25 USDC to Ahmet.");
    const r = explainEvm(parse(E.tx1559({ to: E.ROUTER, data: "0x07ed2379" as Hex, gas: 200_000n, chainId: 1 })), { intel: {}, tokenMeta: {}, addressBook: {}, ownerBalance: null });
    expect(r.spoken).toBe("On Ethereum: Swap through 1inch.");
  });
});

describe("EVM scam filter", () => {
  it("passes a plain transfer to an established wallet", () => {
    const v = assessEvm(parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 10n })), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, {}) } }));
    expect(v.level).toBe("low");
  });
  it("blocks unlimited approve (to anyone), allows exact approve to a router, blocks any approve to an EOA", () => {
    expect(assessEvm(parse(E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.CONTRACT), gas: 60_000n })), ctx({ intel: { [getAddress(E.CONTRACT)]: intel(E.CONTRACT, { isContract: true }) } })).level).toBe("block");
    const toRouter = assessEvm(parse(E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.ROUTER), gas: 60_000n })), ctx());
    expect(toRouter.level).toBe("block"); expect(toRouter.reasons[0]).toMatch(/exact-amount approvals/);
    expect(assessEvm(parse(E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.ROUTER), gas: 60_000n })), ctx({ unlimitedApproveToRouters: true })).level).toBe("medium");
    expect(assessEvm(parse(E.tx1559({ to: E.USDC, data: E.erc20Approve(E.ROUTER, 1000n), gas: 60_000n })), ctx()).level).toBe("low");
    const eoa = assessEvm(parse(E.tx1559({ to: E.USDC, data: E.erc20Approve(E.EOA_SPENDER, 1000n), gas: 60_000n })), ctx({ intel: { [getAddress(E.EOA_SPENDER)]: intel(E.EOA_SPENDER, { isContract: false }) } }));
    expect(eoa.level).toBe("block"); expect(eoa.reasons[0]).toMatch(/plain wallet/);
  });
  it("blocks setApprovalForAll", () => {
    const s = parse(E.tx1559({ to: E.CONTRACT, data: ("0xa22cb465" + "0".repeat(24) + E.EOA_SPENDER.slice(2) + "0".repeat(63) + "1") as Hex, gas: 60_000n }));
    expect(assessEvm(s, ctx()).level).toBe("block");
  });
  it("chain id mismatch with the dapp's request is blocked", () => {
    const v = assessEvm(parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 10n, chainId: 8453 }), 1), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, {}) } }));
    expect(v.level).toBe("block"); expect(v.reasons[0]).toMatch(/asked for chain 1 but the transaction is for chain 8453/);
    expect(assessEvm(parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 10n, chainId: 8453 }), 8453), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, {}) } })).level).toBe("low");
  });
  it("unknown contract with no age and no verification is high; young destination contract is high", () => {
    const u = assessEvm(parse(E.tx1559({ to: E.CONTRACT, data: "0xdeadbeef" as Hex, gas: 100_000n })), ctx({ intel: { [getAddress(E.CONTRACT)]: intel(E.CONTRACT, { isContract: true, ageDays: null, verified: null }) } }));
    expect(u.level).toBe("high"); expect(u.reasons[0]).toMatch(/know nothing about/);
    const y = assessEvm(parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 10n })), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, { isContract: true, ageDays: 1 }) } }));
    expect(y.level).toBe("high");
  });
  it("gas above 10% of value is medium; emptying the wallet is blocked", () => {
    const g = assessEvm(parse(E.tx1559({ to: E.STRANGER, value: E.ETH / 1000n, gas: 21_000n, maxFeePerGas: 10n * E.GWEI })), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, {}) } }));
    expect(g.level).toBe("medium"); expect(g.reasons[0]).toMatch(/fee can reach/);
    const d = assessEvm(parse(E.tx1559({ to: E.STRANGER, value: 95n * E.ETH / 10n })), ctx({ intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, {}) } }));
    expect(d.level).toBe("block"); expect(d.reasons[0]).toMatch(/empties your wallet/);
  });
});

const permit = (value: bigint, deadline: bigint) => ({
  domain: { name: "USD Coin", version: "2", chainId: 1, verifyingContract: E.USDC },
  types: { Permit: [{ name: "owner", type: "address" }, { name: "spender", type: "address" }, { name: "value", type: "uint256" }, { name: "nonce", type: "uint256" }, { name: "deadline", type: "uint256" }] },
  primaryType: "Permit", message: { owner: own, spender: E.ROUTER, value: value.toString(), nonce: "0", deadline: deadline.toString() },
});

describe("EIP-712", () => {
  it("hashes deterministically regardless of key order and matches viem", () => {
    const td = permit(1_000_000n, 1_800_000_000n);
    const shuffled = JSON.parse(JSON.stringify({ message: { deadline: td.message.deadline, nonce: "0", value: td.message.value, spender: E.ROUTER, owner: own }, primaryType: "Permit", types: td.types, domain: { verifyingContract: E.USDC, chainId: 1, version: "2", name: "USD Coin" } }));
    const a = typedDataDigest(parseTypedData(td)); const b = typedDataDigest(parseTypedData(JSON.stringify(shuffled)));
    expect(a).toBe(b);
    expect(canonicalTypedDataJson(parseTypedData(td))).toBe(canonicalTypedDataJson(parseTypedData(shuffled)));
    expect(a).toBe(hashTypedData({ domain: td.domain, types: td.types, primaryType: "Permit", message: { owner: own, spender: E.ROUTER, value: 1_000_000n, nonce: 0n, deadline: 1_800_000_000n } }));
  });
  it("explains and rates permits: unlimited or far-future → high; limited short permit to a router → low", () => {
    const now = 1_700_000_000;
    const bad = parseTypedData(permit(maxUint256, BigInt(now + 10 * 365 * 86_400)));
    expect(permitsOf(bad)[0].unlimited).toBe(true);
    expect(explainTypedData(bad, { nowUnix: now }).spoken).toBe("DANGER: gives 1inch unlimited permission to spend your USDC for 10 years.");
    const v = assessTypedData(bad, { nowUnix: now, routers });
    expect(v.level).toBe("high"); expect(v.reasons.join(" ")).toMatch(/unlimited/); expect(v.reasons.join(" ")).toMatch(/far into the future/);
    const ok = parseTypedData(permit(1_000_000n, BigInt(now + 600)));
    expect(explainTypedData(ok, { nowUnix: now }).spoken).toBe("Lets 1inch spend up to 1 USDC for 1 hours.");
    expect(assessTypedData(ok, { nowUnix: now, routers }).level).toBe("low");
  });
});

describe("EIP-712 listings", () => {
  const seaport = (consideration: object[]) => ({
    domain: { name: "Seaport", version: "1.6", chainId: 1, verifyingContract: "0x0000000000000068F116a894984e2DB1123eB395" },
    types: { OrderComponents: [{ name: "offerer", type: "address" }, { name: "offer", type: "OfferItem[]" }, { name: "consideration", type: "ConsiderationItem[]" }], OfferItem: [{ name: "token", type: "address" }, { name: "startAmount", type: "uint256" }], ConsiderationItem: [{ name: "token", type: "address" }, { name: "startAmount", type: "uint256" }, { name: "recipient", type: "address" }] },
    primaryType: "OrderComponents", message: { offerer: own, offer: [{ token: E.USDC, startAmount: "1" }], consideration },
  });
  it("a Seaport or Blur listing is high; one that asks nothing in return is blocked", () => {
    const paid = assessTypedData(parseTypedData(seaport([{ token: E.WETH, startAmount: "1000000000000000000", recipient: own }])), { routers });
    expect(paid.level).toBe("high"); expect(paid.reasons[0]).toMatch(/lists your NFT/);
    expect(explainTypedData(parseTypedData(seaport([{ token: E.WETH, startAmount: "1", recipient: own }]))).lines[0]).toMatch(/for sale on Seaport/);
    const free = assessTypedData(parseTypedData(seaport([])), { routers });
    expect(free.level).toBe("block"); expect(free.reasons[0]).toMatch(/for nothing/);
    const zero = assessTypedData(parseTypedData(seaport([{ token: E.WETH, startAmount: "0", recipient: own }])), { routers });
    expect(zero.level).toBe("block");
    const blur = (price: string) => parseTypedData({ domain: { name: "Blur Exchange", version: "1.0", chainId: 1, verifyingContract: E.ROUTER }, types: { Order: [{ name: "trader", type: "address" }, { name: "side", type: "uint8" }, { name: "price", type: "uint256" }] }, primaryType: "Order", message: { trader: own, side: 1, price } });
    expect(assessTypedData(blur("0"), { routers }).level).toBe("block");
    expect(assessTypedData(blur("500000000000000000"), { routers }).level).toBe("high");
  });
  it("a typed message whose verifying contract is a plain wallet is blocked", () => {
    const td = parseTypedData({ ...permit(1_000_000n, 1_800_000_000n), domain: { name: "USD Coin", version: "2", chainId: 1, verifyingContract: E.STRANGER } });
    const v = assessTypedData(td, { nowUnix: 1_700_000_000, routers, intel: { [getAddress(E.STRANGER)]: intel(E.STRANGER, { isContract: false, exists: true }) } });
    expect(v.level).toBe("block"); expect(v.reasons.join(" ")).toMatch(/plain wallet, as its contract/);
  });
});

describe("EVM intel", () => {
  it("probes eth_getCode over blocks to date a contract and reads ERC-20 metadata by eth_call", async () => {
    const rpc = E.fakeRpc(1);
    const young = getAddress(E.CONTRACT);
    const createdAt = 20_000_000n - 7200n; // one day of blocks ago
    rpc.getCode = async (a, block) => (a.toLowerCase() === young.toLowerCase() && (block == null || block >= createdAt) ? "0x6001" : "0x");
    const now = () => (1_700_000_000 + 20_000_000 * 12) * 1000;
    const i = new EvmIntel(rpc, { timeoutMs: 1000, now });
    const r = await i.address(young);
    expect(r.isContract).toBe(true); expect(r.ageDays).not.toBeNull(); expect(r.ageDays!).toBeGreaterThan(0.9); expect(r.ageDays!).toBeLessThan(1.1);
    rpc.st.calls["0x95d89b41"] = ("0x" + "20".padStart(64, "0") + "4".padStart(64, "0") + Buffer.from("PEPE").toString("hex").padEnd(64, "0")) as Hex; // symbol()
    rpc.st.calls["0x313ce567"] = ("0x" + "12".padStart(64, "0")) as Hex; // decimals()
    expect(await i.token(E.FRIEND)).toEqual({ symbol: "PEPE", decimals: 18 });
  });
});

describe("EVM pipeline through the sim controller", () => {
  it("explains, speaks, signs; the signed RLP recovers to the owner", async () => {
    const h = E.evmHarness({ rpcState: { balance: { [E.STRANGER.toLowerCase()]: E.ETH }, nonce: { [E.STRANGER.toLowerCase()]: 12 } } });
    const bytes = E.tx1559({ to: E.STRANGER, value: E.ETH / 4n });
    const r = await h.signer.signChain({ chain: "evm", bytes, origin: "https://app.uniswap.org", source: "extension", requestedChainId: 1 });
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    const signed = parseTransaction(("0x" + Buffer.from(r.signedTx).toString("hex")) as Hex);
    expect(signed.r).toBeDefined();
    const digest = keccak256(("0x" + Buffer.from(bytes).toString("hex")) as Hex);
    expect((await recoverAddress({ hash: digest, signature: { r: signed.r!, s: signed.s!, yParity: signed.yParity! } })).toLowerCase()).toBe(h.owner.toLowerCase());
    expect(h.robot.spoken[0]).toBe("From app.uniswap.org, on Ethereum: Sends 0.25 ETH to 0x1234…1234, a wallet with 12+ transactions. Touch me to sign.");
    expect(h.robot.spoken.at(-1)).toBe("Signed.");
  });
  it("legacy transactions assemble with EIP-155 v", async () => {
    const h = E.evmHarness();
    const r = await h.signer.signChain({ chain: "evm", bytes: E.txLegacy({ to: E.STRANGER, value: 1n, chainId: 1 }), source: "api" });
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    const signed = parseTransaction(("0x" + Buffer.from(r.signedTx).toString("hex")) as Hex);
    expect([37n, 38n]).toContain(signed.v);
  });
  it("blocks an unlimited approval without asking for a touch, and a chain-id mismatch", async () => {
    const h = E.evmHarness();
    const r = await h.signer.signChain({ chain: "evm", bytes: E.tx1559({ to: E.USDC, data: E.approveUnlimited(E.EOA_SPENDER), gas: 60_000n }), origin: "https://evil.site", source: "extension" });
    expect(r.ok).toBe(false); if (r.ok) return;
    expect(r.reason).toBe("blocked"); expect(h.robot.spoken[0]).toMatch(/^I'm not signing this\./);
    const m = await h.signer.signChain({ chain: "evm", bytes: E.tx1559({ to: E.STRANGER, value: 1n, chainId: 8453 }), source: "extension", requestedChainId: 1 });
    expect(m.ok).toBe(false); if (!m.ok) expect(m.detail).toMatch(/Something is lying/);
  });
  it("signs EIP-712 permits and EIP-191 messages; refuses a transaction disguised as a message", async () => {
    const h = E.evmHarness();
    const td = permit(1_000_000n, BigInt(Math.floor(Date.now() / 1000) + 600));
    const r = await h.signer.signEvmMessage({ typedData: td, origin: "https://app.uniswap.org" });
    expect(r.ok).toBe(true); if (!r.ok) return;
    const sigHex = ("0x" + Buffer.from(r.signature).toString("hex")) as Hex;
    expect((await recoverTypedDataAddress({ domain: td.domain, types: td.types, primaryType: "Permit", message: { owner: own, spender: E.ROUTER, value: 1_000_000n, nonce: 0n, deadline: BigInt(td.message.deadline) }, signature: sigHex })).toLowerCase()).toBe(h.owner.toLowerCase());
    const m = await h.signer.signEvmMessage({ message: new TextEncoder().encode("Sign in to Uniswap"), origin: "https://app.uniswap.org" });
    expect(m.ok).toBe(true); if (!m.ok) return;
    expect((await recoverMessageAddress({ message: "Sign in to Uniswap", signature: ("0x" + Buffer.from(m.signature).toString("hex")) as Hex })).toLowerCase()).toBe(h.owner.toLowerCase());
    const d = await h.signer.signEvmMessage({ message: E.tx1559({ to: E.STRANGER, value: E.ETH }) });
    expect(d.ok).toBe(false); if (!d.ok) expect(d.reason).toBe("blocked");
  });
});
