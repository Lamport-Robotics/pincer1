import { describe, it, expect } from "vitest";
import { parseMessage, looksLikeMessage, claimsFor } from "../src/tx/parse.js";
import { explain } from "../src/tx/explain.js";
import * as F from "./fixtures.js";
import { SYSTEM, TOKEN, U64_MAX } from "../src/tx/known.js";

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
});
