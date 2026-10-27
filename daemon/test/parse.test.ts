import { describe, it, expect } from "vitest";
import { ownerAtaSet, parseMessage, looksLikeMessage, claimsFor } from "../src/tx/parse.js";
import { explain } from "../src/tx/explain.js";
import * as F from "./fixtures.js";
import { createTransferCheckedInstruction } from "@solana/spl-token";
import { ComputeBudgetProgram, PublicKey, TransactionInstruction } from "@solana/web3.js";
import { SYSTEM, TOKEN, TOKEN_2022, U64_MAX } from "../src/tx/known.js";

const own = F.owner.publicKey.toBase58();

describe("parseMessage", () => {
  it("reads a plain SOL transfer", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 0.5)])), { owner: own });
    expect(p.version).toBe("legacy");
    expect(p.feePayer).toBe(own);
    expect(p.lamportsOut).toBe(500_000_000n);
    expect(p.dest).toBe(F.stranger.toBase58());
    expect(p.programs).toEqual([SYSTEM]);
    expect(p.dangers).toEqual([]);
  });

  it("sums several transfers and picks the biggest recipient", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 0.1), F.transfer(F.friend, 0.3)])), { owner: own });
    expect(p.lamportsOut).toBe(400_000_000n);
    expect(p.dest).toBe(F.friend.toBase58());
  });

  it("ignores transfers that are not from the owner", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 1)])), { owner: F.friend.toBase58() });
    expect(p.lamportsOut).toBe(0n);
  });

  it("counts an inflow only when it lands in the owner's own token account", () => {
    const mine = new PublicKey([...ownerAtaSet(own, F.usdc.toBase58())][0]!);
    const intoMine = createTransferCheckedInstruction(F.strangerUsdc, F.usdc, mine, F.stranger, 5_000_000n, 6);
    const intoTheirs = createTransferCheckedInstruction(F.strangerUsdc, F.usdc, F.friend, F.stranger, 5_000_000n, 6);
    const a = parseMessage(F.messageOf(F.legacy([intoMine])), { owner: own });
    expect(a.tokenIn).toHaveLength(1); expect(a.tokenOut).toHaveLength(0);
    const b = parseMessage(F.messageOf(F.legacy([intoTheirs])), { owner: own });
    expect(b.tokenIn).toHaveLength(0); // "you receive" must not be said for someone else's account
    const c = parseMessage(F.messageOf(F.legacy([intoTheirs])), { owner: own, ownerTokenAccounts: new Set([F.friend.toBase58()]) });
    expect(c.tokenIn).toHaveLength(1);
  });

  it("reads SPL transferChecked with mint and amount", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.usdcTransferChecked(F.strangerUsdc, 25)])), { owner: own });
    expect(p.tokenOut).toHaveLength(1);
    expect(p.tokenOut[0]).toMatchObject({ mint: F.usdc.toBase58(), amount: 25_000_000n, dest: F.strangerUsdc.toBase58() });
    expect(p.programs).toEqual([TOKEN]);
  });

  it("flags setAuthority, unlimited approve and closeAccount as dangers", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.setOwnerAuthority(F.stranger), F.approveUnlimited(F.stranger), F.closeToStranger(F.stranger)])), { owner: own });
    const kinds = p.dangers.map((d) => d.kind);
    expect(kinds).toEqual(["set_authority", "approve", "close_account"]);
    expect(p.dangers[0].to).toBe(F.stranger.toBase58());
    expect(p.dangers[1].amount).toBe(U64_MAX);
  });

  it("flags handing a nonce account over, and any System instruction it does not know", () => {
    const nonce = F.friend; // pretend this is the owner's nonce account
    const authorize = new TransactionInstruction({ programId: new PublicKey(SYSTEM), keys: [{ pubkey: nonce, isSigner: false, isWritable: true }, { pubkey: F.owner.publicKey, isSigner: true, isWritable: false }], data: Buffer.concat([Buffer.from([7, 0, 0, 0]), F.stranger.toBuffer()]) });
    const p = parseMessage(F.messageOf(F.legacy([authorize])), { owner: own });
    expect(p.dangers.map((d) => d.kind)).toEqual(["set_authority"]);
    expect(claimsFor(p).dangerous).toEqual(["set_authority"]);
    const odd = new TransactionInstruction({ programId: new PublicKey(SYSTEM), keys: [{ pubkey: F.owner.publicKey, isSigner: true, isWritable: true }], data: Buffer.from([99, 0, 0, 0]) });
    const q = parseMessage(F.messageOf(F.legacy([odd])), { owner: own });
    expect(q.dangers.map((d) => d.kind)).toEqual(["unknown_instruction"]);
  });

  it("parses v0 messages and marks unresolved lookup tables", () => {
    const table = new F.AddressLookupTableAccount({ key: F.friend, state: { deactivationSlot: 0n, lastExtendedSlot: 0, lastExtendedSlotStartIndex: 0, authority: undefined, addresses: [F.stranger, F.strangerUsdc] } });
    const bytes = F.messageOf(F.v0([F.transfer(F.stranger, 0.2)], [table]));
    const unresolved = parseMessage(bytes, { owner: own });
    expect(unresolved.version).toBe(0);
    expect(unresolved.unresolvedLookups).toBe(true);
    expect(unresolved.dangers.some((d) => d.kind === "lookup_table")).toBe(true);
    const resolved = parseMessage(bytes, { owner: own, lookupTables: { [F.friend.toBase58()]: [F.stranger.toBase58(), F.strangerUsdc.toBase58()] } });
    expect(resolved.unresolvedLookups).toBe(false);
    expect(resolved.lamportsOut).toBe(200_000_000n);
    expect(resolved.dest).toBe(F.stranger.toBase58());
  });

  it("claimsFor mirrors the controller contract", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 0.5), F.usdcTransferChecked(F.strangerUsdc, 1)])), { owner: own });
    const c = claimsFor(p);
    expect(c.lamports_out).toBe("500000000");
    expect(c.token_out[0]).toEqual({ mint: F.usdc.toBase58(), amount: "1000000", dest: F.strangerUsdc.toBase58() });
    expect(c.programs).toEqual([SYSTEM, TOKEN]);
    expect(c.dangerous).toEqual([]);
    // a program the controller does not know is declared so the controller's own rule can bite
    expect(claimsFor(parseMessage(F.messageOf(F.legacy([F.callUnknown()])), { owner: own })).dangerous).toEqual(["unknown_program"]);
    expect(claimsFor(parseMessage(F.messageOf(F.legacy([F.jupiterLike()])), { owner: own })).dangerous).toEqual([]);
  });

  it("looksLikeMessage distinguishes messages from arbitrary bytes", () => {
    expect(looksLikeMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 0.5)])))).toBe(true);
    expect(looksLikeMessage(new TextEncoder().encode("Sign in to pump.fun\nnonce: 1234"))).toBe(false);
  });
});

describe("explain", () => {
  it("speaks a transfer in one sentence", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 0.5)])), { owner: own });
    const e = explain(p, { origin: "https://pump.fun" });
    expect(e.spoken).toBe(`From pump.fun: Sends 0.5 SOL to ${F.stranger.toBase58().slice(0, 4)}…${F.stranger.toBase58().slice(-4)}.`);
  });
  it("uses the address book and intel", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.transfer(F.friend, 1)])), { owner: own });
    expect(explain(p, { addressBook: { [F.friend.toBase58()]: "Ahmet" } }).spoken).toBe("Sends 1 SOL to Ahmet.");
    const e = explain(p, { intel: { [F.friend.toBase58()]: { address: F.friend.toBase58(), exists: true, isProgram: false, ageDays: 2.5, txCount: 1, txCountIsLowerBound: false, lamports: 0, fetchedAt: 0 } } });
    expect(e.spoken).toContain("a wallet 2 days old with 1 transactions");
  });
  it("names known tokens and dangers", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.usdcTransferChecked(F.strangerUsdc, 25), F.approveUnlimited(F.stranger)])), { owner: own });
    const e = explain(p);
    expect(e.lines[0]).toContain("sends 25 USDC");
    expect(e.lines.some((l) => l.startsWith("DANGER: unlimited token delegation"))).toBe(true);
  });
  it("describes a swap", () => {
    const p = parseMessage(F.messageOf(F.legacy([F.jupiterLike()])), { owner: own });
    expect(explain(p).spoken).toBe("Swap through Jupiter.");
  });

  it("counts the compute budget as a fee, claims it, and treats a huge priority fee as a transfer", () => {
    const p = parseMessage(F.messageOf(F.legacy([ComputeBudgetProgram.setComputeUnitLimit({ units: 1_400_000 }), ComputeBudgetProgram.setComputeUnitPrice({ microLamports: 1_000_000_000_000n }), F.transfer(F.stranger, 0)])), { owner: own });
    expect(p.lamportsOut).toBe(0n);
    expect(p.priorityLamports).toBe(1_400_000_000_000n);
    expect(p.feeLamports).toBe(1_400_000_000_000n + 5000n);
    expect(claimsFor(p).fee_lamports).toBe((1_400_000_000_000n + 5000n).toString());
    // no explicit limit: 200k units per non-budget instruction
    const q = parseMessage(F.messageOf(F.legacy([ComputeBudgetProgram.setComputeUnitPrice({ microLamports: 1000 }), F.transfer(F.stranger, 1)])), { owner: own });
    expect(q.priorityLamports).toBe(200n);
    expect(q.feeLamports).toBe(5200n);
    const plain = parseMessage(F.messageOf(F.legacy([F.transfer(F.stranger, 1)])), { owner: own });
    expect(plain.priorityLamports).toBe(0n);
    expect(plain.feeLamports).toBe(5000n);
  });

  it("Token-2022: transferCheckedWithFee and burnChecked are outflows; unclassified tags are declared, benign ones are not", () => {
    const keys = (owner: boolean) => [
      { pubkey: F.ownerUsdc, isSigner: false, isWritable: true },
      { pubkey: F.usdc, isSigner: false, isWritable: false },
      { pubkey: F.strangerUsdc, isSigner: false, isWritable: true },
      { pubkey: owner ? F.owner.publicKey : F.friend, isSigner: true, isWritable: false },
    ];
    const withFee = new Uint8Array(19); withFee[0] = 26; withFee[1] = 1; new DataView(withFee.buffer).setBigUint64(2, 777n, true); withFee[10] = 6; new DataView(withFee.buffer).setBigUint64(11, 7n, true);
    const p = parseMessage(F.messageOf(F.legacy([new TransactionInstruction({ programId: new PublicKey(TOKEN_2022), keys: keys(true), data: Buffer.from(withFee) })])), { owner: own });
    expect(p.tokenOut).toEqual([{ mint: F.usdc.toBase58(), amount: 777n, source: F.ownerUsdc.toBase58(), dest: F.strangerUsdc.toBase58() }]);
    expect(p.dangers).toEqual([]);
    const burn = new Uint8Array(10); burn[0] = 15; new DataView(burn.buffer).setBigUint64(1, 55n, true); burn[9] = 6;
    const b = parseMessage(F.messageOf(F.legacy([new TransactionInstruction({ programId: new PublicKey(TOKEN_2022), keys: keys(true).slice(0, 2).concat([keys(true)[3]!]), data: Buffer.from(burn) })])), { owner: own });
    expect(b.tokenOut).toEqual([{ mint: F.usdc.toBase58(), amount: 55n, source: F.ownerUsdc.toBase58(), dest: "" }]);
    expect(claimsFor(b).token_out).toEqual([{ mint: F.usdc.toBase58(), amount: "55", dest: "" }]);
    expect(claimsFor(b).dangerous).toEqual([]); // the controller has no "burn" word: the outflow is the claim
    for (const tag of [27, 38, 35]) {
      const u = parseMessage(F.messageOf(F.legacy([new TransactionInstruction({ programId: new PublicKey(TOKEN_2022), keys: keys(true).slice(0, 3), data: Buffer.from([tag, 3]) })])), { owner: own });
      expect(u.dangers.map((d) => d.kind)).toEqual(["unknown_instruction"]);
    }
    const harvest = parseMessage(F.messageOf(F.legacy([new TransactionInstruction({ programId: new PublicKey(TOKEN_2022), keys: keys(true).slice(0, 3), data: Buffer.from([26, 4]) })])), { owner: own });
    expect(harvest.dangers.map((d) => d.kind)).toEqual(["unknown_instruction"]);
    const sync = parseMessage(F.messageOf(F.legacy([new TransactionInstruction({ programId: new PublicKey(TOKEN), keys: keys(true).slice(0, 1), data: Buffer.from([17]) })])), { owner: own });
    expect(sync.dangers).toEqual([]);
  });

});
