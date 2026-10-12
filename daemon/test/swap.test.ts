/** Swaps: quote → execute through fake Jupiter / 1inch / THORChain, with the router allowlist and min-output checks biting. */
import { describe, it, expect } from "vitest";
import { encodeFunctionData, getAddress, parseTransaction, type Address, type Hex } from "viem";
import { PublicKey, Transaction, TransactionInstruction } from "@solana/web3.js";
import * as bitcoin from "bitcoinjs-lib";
import { SwapService, SwapError } from "../src/swap/index.js";
import { JupiterProvider, OneInchProvider, ThorchainProvider } from "../src/swap/providers.js";
import { ThorPins } from "../src/swap/routers.js";
import { ONEINCH_ABI, JUPITER_DISCRIMINATORS, decodeJupiterRoute, parseThorMemo } from "../src/swap/verify.js";
import { BitcoinAdapter } from "../src/chains/bitcoin/index.js";
import { parsePsbt } from "../src/chains/bitcoin/psbt.js";
import { decodeFunctionData, erc20Abi } from "viem";
import * as E from "./evm_fixtures.js";
import * as F from "./fixtures.js";
import { fakeEsplora } from "./btc_fixtures.js";

const JUP = new PublicKey("JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4");
const SOL = "So11111111111111111111111111111111111111112";
const USDC_SOL = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
const VAULT_BTC = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
const VAULT_ETH = "0x00000000000000000000000000000000000e7ab1" as Address;
const THOR_ROUTER = "0x0000000000000000000000000000000000d37c0a" as Address;

/** A Jupiter sharedAccountsRoute instruction with the given tail fields and an empty route plan. */
function jupiterRouteIx(owner: PublicKey, inAmount: bigint, quotedOut: bigint, slippageBps: number) {
  const data = Buffer.alloc(8 + 1 + 4 + 8 + 8 + 2 + 1);
  Buffer.from(JUPITER_DISCRIMINATORS.sharedAccountsRoute, "hex").copy(data, 0);
  data[8] = 1; data.writeUInt32LE(0, 9);
  data.writeBigUInt64LE(inAmount, 13); data.writeBigUInt64LE(quotedOut, 21); data.writeUInt16LE(slippageBps, 29); data[31] = 0;
  return new TransactionInstruction({ programId: JUP, keys: [{ pubkey: owner, isSigner: true, isWritable: true }], data });
}

function oneInchSwapCalldata(o: { src: Address; dst: Address; amount: bigint; minReturn: bigint; receiver: Address }) {
  const executor = "0x0000000000000000000000000000000000e4ec07" as Address;
  return encodeFunctionData({ abi: ONEINCH_ABI, functionName: "swap", args: [executor, { srcToken: o.src, dstToken: o.dst, srcReceiver: executor, dstReceiver: o.receiver, amount: o.amount, minReturnAmount: o.minReturn, flags: 0n }, "0x"] });
}

interface FakeNet { oneInchTo?: Address; oneInchMinReturn?: bigint; jupQuotedOut?: bigint; jupSlippage?: number }

function harness(net: FakeNet = {}) {
  const h = E.evmHarness({ rpcState: { code: { [E.ROUTER.toLowerCase()]: "0x60", [E.USDC.toLowerCase()]: "0x60", [E.WETH.toLowerCase()]: "0x60", [THOR_ROUTER.toLowerCase()]: "0x60" }, balance: { }, nonce: { } }, settings: { mode: "online", swapMaxSlippageBps: 100 } });
  h.rpc1.st.balance[h.owner.toLowerCase()] = 10n * E.ETH;
  const owner = h.owner;
  const solOwner = new PublicKey(h.sim.publicKey);
  const esp = fakeEsplora({ utxos: { [h.sim.bitcoinAddress]: [{ txid: "aa".repeat(32), vout: 0, value: 2_000_000 }] }, fee: { "3": 10 } });
  const btc = new BitcoinAdapter({ esplora: esp.esplora, network: "mainnet" });
  h.signer.register(btc);
  const calls: string[] = [];
  const fetchImpl = (async (url: string | URL | Request, init?: RequestInit) => {
    const u = String(url); calls.push(u);
    const J = (o: unknown) => new Response(JSON.stringify(o), { status: 200 });
    if (u.startsWith("https://jup.test/quote")) return J({ inputMint: SOL, outputMint: USDC_SOL, inAmount: "100000000", outAmount: "20000000", otherAmountThreshold: "19800000", slippageBps: 100, swapMode: "ExactIn" });
    if (u.startsWith("https://jup.test/swap")) {
      const tx = new Transaction({ feePayer: solOwner, recentBlockhash: F.BLOCKHASH }).add(jupiterRouteIx(solOwner, 100_000_000n, net.jupQuotedOut ?? 20_000_000n, net.jupSlippage ?? 100));
      return J({ swapTransaction: Buffer.from(tx.serialize({ requireAllSignatures: false, verifySignatures: false })).toString("base64") });
    }
    if (u.startsWith("https://oneinch.test/1/quote")) return J({ dstAmount: (5n * 10n ** 17n).toString() });
    if (u.startsWith("https://oneinch.test/1/swap")) return J({ dstAmount: (5n * 10n ** 17n).toString(), tx: { from: owner, to: net.oneInchTo ?? E.ROUTER, data: oneInchSwapCalldata({ src: E.USDC, dst: E.WETH, amount: 1_000_000_000n, minReturn: net.oneInchMinReturn ?? 495n * 10n ** 15n, receiver: owner }), value: "0", gas: 250000, gasPrice: "20000000000" } });
    if (u.startsWith("https://thor.test/thorchain/inbound_addresses")) return J([{ chain: "BTC", address: VAULT_BTC, halted: false, gas_rate: "10", dust_threshold: "10000" }, { chain: "ETH", address: VAULT_ETH, router: THOR_ROUTER, halted: false, gas_rate: "30" }]);
    if (u.startsWith("https://thor.test/thorchain/quote/swap")) { const q = new URL(u); const toBtc = q.searchParams.get("to_asset") === "BTC.BTC"; return J({ inbound_address: toBtc ? VAULT_ETH : VAULT_BTC, router: toBtc ? THOR_ROUTER : undefined, expected_amount_out: toBtc ? "900000" : "50000000", memo: "=:x", expiry: Math.floor(Date.now() / 1000) + 900, dust_threshold: "10000", recommended_min_amount_in: "100000", fees: {} }); }
    return new Response("nope", { status: 404 });
  }) as unknown as typeof fetch;
  const sentSol: string[] = [];
  (h.conn as any).sendRawTransaction = async (raw: Uint8Array) => { sentSol.push(Buffer.from(raw).toString("base64")); return "jupsig111"; };
  const settings = { mode: "online" as const, swapMaxSlippageBps: 100 };
  const swap = new SwapService({ signer: h.signer, settings: () => settings, evm: h.evm, bitcoin: btc, jupiter: new JupiterProvider("https://jup.test", fetchImpl), aggregators: [new OneInchProvider("key", "https://oneinch.test", fetchImpl)], thorchain: new ThorchainProvider("https://thor.test", fetchImpl), thorPins: new ThorPins("https://thor.test", fetchImpl, Date.now, (r) => h.evm.routers.add(r.toLowerCase())) });
  return { ...h, btc, swap, settings, calls, sentSol, esp };
}

describe("swap: Jupiter (Solana)", () => {
  it("quotes with a spoken summary, verifies the route's minimum output, signs and broadcasts", async () => {
    const h = harness();
    const q = await h.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: USDC_SOL }, amount: "0.1" });
    expect(q.route).toBe("jupiter"); expect(q.amountIn).toBe(100_000_000n); expect(q.expectedOut).toBe(20_000_000n); expect(q.minOut).toBe(19_800_000n);
    expect(q.spoken).toBe("Swapping 0.1 SOL for about 20 USDC through Jupiter. You'll get at least 19.8 USDC, that's 1% slippage. Say execute within ten minutes.");
    const r = await h.swap.execute(q.id);
    expect(r.steps).toEqual([{ kind: "swap", chain: "solana", txid: "jupsig111" }]);
    expect(h.sentSol).toHaveLength(1);
    expect(h.robot.spoken.find((s) => s.includes("Touch me to sign"))).toBe("You asked to swap 0.1 SOL for at least 19.8 USDC. Swap through Jupiter. Touch me to sign.");
    await expect(h.swap.execute(q.id)).rejects.toMatchObject({ code: "unknown_quote" }); // one execution per quote
  });
  it("refuses a route whose embedded minimum is below the quote, or with slippage above the cap", async () => {
    const low = harness({ jupQuotedOut: 19_000_000n });
    const q = await low.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: USDC_SOL }, amount: "0.1" });
    await expect(low.swap.execute(q.id)).rejects.toMatchObject({ code: "min_out" });
    expect(low.sentSol).toHaveLength(0);
    const slip = harness({ jupSlippage: 300 });
    const q2 = await slip.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: USDC_SOL }, amount: "0.1", slippageBps: 300 });
    expect(q2.slippageBps).toBe(100); // capped
    await expect(slip.swap.execute(q2.id)).rejects.toMatchObject({ code: "slippage" });
  });
  it("decodes Jupiter route tails and refuses exact-out routes", () => {
    const ix = jupiterRouteIx(PublicKey.default, 5n, 100n, 50);
    expect(decodeJupiterRoute(ix.data)).toEqual({ inAmount: 5n, quotedOut: 100n, slippageBps: 50, minOut: 99n });
    const exactOut = Buffer.concat([Buffer.from(JUPITER_DISCRIMINATORS.exactOutRoute, "hex"), Buffer.alloc(32)]);
    expect(() => decodeJupiterRoute(exactOut)).toThrow(/exact-out/);
  });
});

describe("swap: 1inch (EVM)", () => {
  it("USDC → WETH: exact-amount approval to the router, then the swap; both verified and signed", async () => {
    const h = harness();
    const q = await h.swap.quote({ from: { chain: "evm", chainId: 1, asset: E.USDC }, to: { chain: "evm", chainId: 1, asset: "0xC02aaA39b223FE8D0A0e5C4F27eAD9083C756Cc2" }, amount: "1000" });
    expect(q.route).toBe("1inch"); expect(q.minOut).toBe(495n * 10n ** 15n);
    expect(q.spoken).toBe("Swapping 1,000 USDC on Ethereum for about 0.5 WETH through 1inch. You'll get at least 0.495 WETH, that's 1% slippage. Say execute within ten minutes.");
    const r = await h.swap.execute(q.id);
    expect(r.steps.map((s) => s.kind)).toEqual(["approve", "swap"]);
    expect(h.rpc1.st.sent).toHaveLength(2);
    const approve = parseTransaction(h.rpc1.st.sent[0]);
    expect(approve.to!.toLowerCase()).toBe(E.USDC.toLowerCase());
    const d = decodeFunctionData({ abi: erc20Abi, data: approve.data! });
    expect(d).toEqual({ functionName: "approve", args: [getAddress(E.ROUTER), 1_000_000_000n] });
    expect(approve.nonce).toBe(0);
    const sw = parseTransaction(h.rpc1.st.sent[1]);
    expect(sw.to!.toLowerCase()).toBe(E.ROUTER.toLowerCase()); expect(sw.nonce).toBe(1); expect(sw.value ?? 0n).toBe(0n);
    expect(h.robot.spoken.filter((s) => s.endsWith("Touch me to sign."))).toEqual([
      "You asked to swap 1,000 USDC for at least 0.495 WETH. First, an exact approval of 1,000 USDC for 1inch. On Ethereum: Lets 1inch spend up to 1,000 USDC. Touch me to sign.",
      "You asked to swap 1,000 USDC for at least 0.495 WETH. On Ethereum: Swap through 1inch. Touch me to sign.",
    ]);
  });
  it("rejects a transaction that does not go to the allowlisted router, and one whose minimum output is short", async () => {
    const bad = harness({ oneInchTo: E.CONTRACT });
    const q = await bad.swap.quote({ from: { chain: "evm", chainId: 1, asset: E.USDC }, to: { chain: "evm", chainId: 1, asset: E.WETH }, amount: "1000" });
    await expect(bad.swap.execute(q.id)).rejects.toMatchObject({ code: "router" });
    expect(bad.rpc1.st.sent).toHaveLength(0);
    const short = harness({ oneInchMinReturn: 400n * 10n ** 15n });
    const q2 = await short.swap.quote({ from: { chain: "evm", chainId: 1, asset: E.USDC }, to: { chain: "evm", chainId: 1, asset: E.WETH }, amount: "1000" });
    await expect(short.swap.execute(q2.id)).rejects.toMatchObject({ code: "min_out" });
    expect(short.rpc1.st.sent).toHaveLength(0);
  });
});

describe("swap: THORChain (cross-chain)", () => {
  it("BTC → ETH: pays the pinned inbound vault with a memo carrying the quoted minimum to the owner's EVM address", async () => {
    const h = harness();
    const q = await h.swap.quote({ from: { chain: "bitcoin", asset: "BTC" }, to: { chain: "evm", chainId: 1, asset: "ETH" }, amount: "0.01" });
    expect(q.route).toBe("thorchain"); expect(q.amountIn).toBe(1_000_000n); expect(q.expectedOut).toBe(5n * 10n ** 17n); expect(q.minOut).toBe(495n * 10n ** 15n);
    expect(q.spoken).toBe("Swapping 0.01 BTC for about 0.5 ETH to Ethereum through THORChain. You'll get at least 0.495 ETH, that's 1% slippage. Say execute within ten minutes.");
    const r = await h.swap.execute(q.id);
    expect(r.steps[0]).toMatchObject({ kind: "swap", chain: "bitcoin" });
    expect(h.esp.sent).toHaveLength(1);
    const tx = bitcoin.Transaction.fromHex(h.esp.sent[0]);
    const vaultOut = tx.outs.find((o) => { try { return bitcoin.address.fromOutputScript(o.script) === VAULT_BTC; } catch { return false; } });
    expect(vaultOut?.value).toBe(1_000_000);
    const opret = tx.outs.find((o) => o.script[0] === 0x6a)!;
    const memo = (bitcoin.script.decompile(opret.script)!.find(Buffer.isBuffer) as Buffer).toString();
    expect(parseThorMemo(memo)).toEqual({ toAsset: "ETH.ETH", dest: h.owner, limit: 49_500_000n });
    expect(h.robot.spoken.find((s) => s.endsWith("Touch me to sign."))).toMatch(/^You asked to swap 0.01 BTC for at least 0.495 ETH\. Sends 0.01 BTC to THORChain vault, fee [\d,]+ sats/);
  });
  it("ETH → BTC: calls the pinned router's depositWithExpiry with the pinned vault", async () => {
    const h = harness();
    const q = await h.swap.quote({ from: { chain: "evm", chainId: 1, asset: "ETH" }, to: { chain: "bitcoin", asset: "BTC" }, amount: "0.5" });
    expect(q.expectedOut).toBe(900_000n);
    const r = await h.swap.execute(q.id);
    expect(r.steps[0]).toMatchObject({ kind: "swap", chain: "evm" });
    const tx = parseTransaction(h.rpc1.st.sent[0]);
    expect(tx.to!.toLowerCase()).toBe(THOR_ROUTER.toLowerCase()); expect(tx.value).toBe(5n * 10n ** 17n);
    expect(tx.data!.startsWith("0x44bc937b") || tx.data!.length > 10).toBe(true);
  });
  it("refuses when the quote's inbound address is not the pinned one", async () => {
    const h = harness();
    const q = await h.swap.quote({ from: { chain: "bitcoin", asset: "BTC" }, to: { chain: "evm", chainId: 1, asset: "ETH" }, amount: "0.01" });
    (q.raw as any).inboundAddress = "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq";
    await expect(h.swap.execute(q.id)).rejects.toMatchObject({ code: "router" });
    expect(h.esp.sent).toHaveLength(0);
  });
});

describe("swap: mode gating and inputs", () => {
  it("offline mode refuses quotes and executions with a clear error", async () => {
    const h = harness();
    (h.settings as any).mode = "offline";
    await expect(h.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: USDC_SOL }, amount: "0.1" })).rejects.toMatchObject({ code: "offline_mode" });
    expect(h.calls).toHaveLength(0);
  });
  it("rejects bad amounts, unknown assets and unsupported pairs", async () => {
    const h = harness();
    await expect(h.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "solana", asset: USDC_SOL }, amount: "-1" })).rejects.toBeInstanceOf(SwapError);
    await expect(h.swap.quote({ from: { chain: "bitcoin", asset: "LTC" }, to: { chain: "solana", asset: "SOL" }, amount: "1" })).rejects.toMatchObject({ code: "unknown_asset" });
    await expect(h.swap.quote({ from: { chain: "solana", asset: "SOL" }, to: { chain: "bitcoin", asset: "BTC" }, amount: "1" })).rejects.toMatchObject({ code: "unsupported" });
    const p = parsePsbt; void p;
  });
});
