/** The public corpus in /vectors: real mainnet transactions with expectations read off the chain, parsed by
 * the real chain adapters. Same files, same expectations as firmware/test/test_vectors.c. */
import { describe, it, expect } from "vitest";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { getAddress } from "viem";
import { parseMessage, claimsFor } from "../src/tx/parse.js";
import { parseEvmTx } from "../src/chains/evm/parse.js";
import { EvmAdapter } from "../src/chains/evm/index.js";
import { parseTypedData, typedDataDigest, permitsOf, listingOf } from "../src/chains/evm/message.js";
import { parsePsbt } from "../src/chains/bitcoin/psbt.js";
import { BitcoinAdapter } from "../src/chains/bitcoin/index.js";
import { fakeEsplora } from "./btc_fixtures.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "vectors");
const load = (chain: string) => readdirSync(join(ROOT, chain)).filter((f) => f.endsWith(".json")).sort().map((f) => JSON.parse(readFileSync(join(ROOT, chain, f), "utf8")));
const bytesOf = (v: any) => new Uint8Array(Buffer.from(v.bytes, "base64"));
const set = (xs: string[]) => [...new Set(xs)].sort();

describe("vectors: solana", () => {
  for (const v of load("solana")) {
    it(v.name, () => {
      const p = parseMessage(bytesOf(v), { owner: v.owner, lookupTables: v.lookup_tables });
      expect(p.version).toBe(v.expect.version);
      expect(p.feePayer).toBe(v.owner);
      expect(p.signers).toHaveLength(v.expect.signers);
      expect(p.lamportsOut.toString()).toBe(v.expect.lamports_out);
      if (BigInt(v.expect.lamports_out) > 0n) expect(p.dest).toBe(v.expect.dest);
      expect(p.tokenOut.map((t) => ({ dest: t.dest, amount: t.amount.toString() }))).toEqual(v.expect.token_out.map((t: any) => ({ dest: t.dest, amount: t.amount })));
      for (const [i, t] of v.expect.token_out.entries()) if (t.mint_in_instruction) expect(p.tokenOut[i].mint).toBe(t.mint);
      expect(set(p.programs)).toEqual(set(v.expect.programs));
      expect(p.feeLamports.toString()).toBe(v.expect.fee_lamports);
      expect(p.usesDurableNonce).toBe(v.expect.durable_nonce);
      expect(p.unresolvedLookups).toBe(false); // the vectors carry their lookup tables
      // `warnings` are the transaction's own; a message with lookup tables is also `accept_with_declaration:
      // lookup_table` for the controller, which cannot read them
      expect(set(claimsFor(p).dangerous)).toEqual(set(v.lookup_tables ? [...v.expect.warnings, "lookup_table"] : v.expect.warnings));
    });
  }
});

describe("vectors: evm", () => {
  const evm = new EvmAdapter({ rpcs: new Map() });
  const CALL: Record<string, string> = { none: "unknown", transfer: "transfer", approve: "approve", transferFrom: "transferFrom", setApprovalForAll: "setApprovalForAll", increaseAllowance: "increaseAllowance", create: "create", router: "router", unknown: "unknown" };
  for (const v of load("evm")) {
    it(v.name, () => {
      const p = parseEvmTx(bytesOf(v), { owner: v.owner }, { routers: evm.routers });
      expect(p.type).toBe(v.expect.type);
      expect(p.chainId).toBe(v.expect.chain_id);
      expect(p.nonce).toBe(v.expect.nonce);
      expect(p.to).toBe(v.expect.to);
      expect(p.value.toString()).toBe(v.expect.value_wei);
      expect(p.gas.toString()).toBe(v.expect.gas_limit);
      expect(p.maxFeePerGas.toString()).toBe(v.expect.max_fee_per_gas_wei);
      expect(p.maxFeeWei.toString()).toBe(v.expect.max_fee_wei);
      expect(p.call.fn).toBe(CALL[v.expect.call]);
      if (v.expect.call === "none") expect((p.call as any).selector).toBeNull();
      expect(p.tokenOut.map((t) => ({ token: t.token, to: t.dest, amount: t.amount.toString() }))).toEqual(v.expect.token_out);
      if (v.expect.spender) { expect((p.call as any).spender).toBe(getAddress(v.expect.spender)); expect((p.call as any).amount.toString()).toBe(v.expect.allowance); }
      if (v.expect.call === "transfer" || v.expect.call === "transferFrom" || v.expect.call === "setApprovalForAll") expect((p.call as any).to ?? (p.call as any).operator).toBe(v.expect.call_to);
      expect(set(evm.claims(p).dangerous)).toEqual(set(v.expect.warnings));
    });
  }
});

describe("vectors: bitcoin", () => {
  const btc = new BitcoinAdapter({ esplora: fakeEsplora().esplora, network: "mainnet" });
  for (const v of load("bitcoin")) {
    it(v.name, () => {
      if (v.expect.controller.startsWith("refuse")) { expect(() => parsePsbt(bytesOf(v), "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", "mainnet")).toThrow(); return; }
      const p = parsePsbt(bytesOf(v), v.owner.address, "mainnet");
      expect(p.ownerInputs).toHaveLength(v.expect.owner_inputs);
      expect(p.foreignInputs).toBe(0);
      expect(p.outputs).toHaveLength(v.expect.outputs);
      expect(p.satsOut.toString()).toBe(v.expect.sats_out);
      expect(p.feeSats.toString()).toBe(v.expect.fee_sats);
      expect(p.changeSats.toString()).toBe(v.expect.change_sats);
      expect(p.dest).toBe(v.expect.dest);
      expect(p.sighashAll).toBe(true);
      expect(set(btc.claims(p).dangerous)).toEqual(set(v.expect.warnings));
    });
  }
});

describe("vectors: eip-712", () => {
  for (const v of load("evm_typed")) {
    it(v.name, () => {
      const td = parseTypedData(v.typed_data);
      expect(typedDataDigest(td)).toBe(v.expect.digest);
      const permits = permitsOf(td);
      if (v.expect.spender) {
        expect(permits).toHaveLength(1);
        expect(permits[0].spender.toLowerCase()).toBe(v.expect.spender.toLowerCase());
        expect(permits[0].amount.toString()).toBe(v.expect.allowance);
        expect(permits[0].unlimited).toBe(v.expect.warnings.includes("approve_unlimited"));
      } else expect(permits).toEqual([]);
      expect(listingOf(td)?.market ?? null).toBe(v.expect.market ?? null);
    });
  }
});
