import { describe, it, expect } from "vitest";
import { parseMessage } from "../src/tx/parse.js";
import { assess, isLookalike } from "../src/risk/filter.js";
import { originAllowed } from "../src/risk/filter.js";
import type { AddressIntel } from "../src/risk/intel.js";
import * as F from "./fixtures.js";

const own = F.owner.publicKey.toBase58();
const intel = (address: string, o: Partial<AddressIntel>): AddressIntel => ({ address, exists: true, isProgram: false, ageDays: 400, txCount: 500, txCountIsLowerBound: false, lamports: 1e9, fetchedAt: 0, ...o });
const parse = (ixs: any[]) => parseMessage(F.messageOf(F.legacy(ixs)), { owner: own });

describe("scam filter", () => {
  it("passes an ordinary transfer to an established wallet", () => {
    const v = assess({ tx: parse([F.transfer(F.stranger, 0.5)]), owner: own, ownerLamports: 10e9, intel: { [F.stranger.toBase58()]: intel(F.stranger.toBase58(), {}) } });
    expect(v.level).toBe("low");
  });

  it("blocks setAuthority (the drainer move)", () => {
    const v = assess({ tx: parse([F.setOwnerAuthority(F.stranger)]), owner: own });
    expect(v.level).toBe("block");
    expect(v.reasons[0]).toMatch(/hands over control/);
  });

  it("blocks unlimited approve to a plain wallet, only warns for a program", () => {
    const toWallet = assess({ tx: parse([F.approveUnlimited(F.stranger)]), owner: own, intel: { [F.stranger.toBase58()]: intel(F.stranger.toBase58(), { isProgram: false }) } });
    expect(toWallet.level).toBe("block");
    const toProgram = assess({ tx: parse([F.approveUnlimited(F.stranger)]), owner: own, intel: { [F.stranger.toBase58()]: intel(F.stranger.toBase58(), { isProgram: true }) } });
    expect(toProgram.level).toBe("high");
  });

  it("blocks closeAccount with rent to a stranger", () => {
    expect(assess({ tx: parse([F.closeToStranger(F.stranger)]), owner: own }).level).toBe("block");
    expect(assess({ tx: parse([F.closeToStranger(F.friend)]), owner: own, addressBook: { [F.friend.toBase58()]: "me2" } }).level).toBe("low");
  });

  it("warns on brand-new destinations and blocks wallet-emptying sends", () => {
    const fresh = { [F.stranger.toBase58()]: intel(F.stranger.toBase58(), { ageDays: 0.4, txCount: 1 }) };
    const v = assess({ tx: parse([F.transfer(F.stranger, 0.5)]), owner: own, ownerLamports: 10e9, intel: fresh });
    expect(v.level).toBe("high");
    expect(v.reasons[0]).toMatch(/less than a day old/);
    const drain = assess({ tx: parse([F.transfer(F.stranger, 9.8)]), owner: own, ownerLamports: 10e9, intel: fresh });
    expect(drain.level).toBe("block");
    expect(drain.reasons.some((r) => r.includes("empties your wallet"))).toBe(true);
  });

  it("detects address poisoning against the address book", () => {
    const real = F.friend.toBase58();
    const fake = real.slice(0, 4) + "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx".slice(0, real.length - 8) + real.slice(-4);
    expect(isLookalike(fake, real)).toBe(true);
    expect(isLookalike(real, real)).toBe(false);
    const tx = parse([F.transfer(F.stranger, 0.1)]);
    tx.dest = fake; // simulate: the destination is a poisoned lookalike
    const v = assess({ tx, owner: own, addressBook: { [real]: "Ahmet" } });
    expect(v.level).toBe("block");
    expect(v.reasons[0]).toMatch(/address poisoning/);
  });

  it("marks unknown programs medium, brand-new programs high, non-executable 'programs' block", () => {
    const tx = parse([F.callUnknown()]);
    const u = F.unknownProgram.toBase58();
    expect(assess({ tx, owner: own }).level).toBe("medium");
    expect(assess({ tx, owner: own, intel: { [u]: intel(u, { isProgram: true, ageDays: 2 }) } }).level).toBe("high");
    expect(assess({ tx, owner: own, intel: { [u]: intel(u, { isProgram: false }) } }).level).toBe("block");
    expect(assess({ tx, owner: own, knownPrograms: new Set([u]) }).level).toBe("low");
    // with value leaving the wallet an unapproved program is a warning, every time
    const withValue = parse([F.transfer(F.stranger, 0.1), F.callUnknown()]);
    expect(assess({ tx: withValue, owner: own }).level).toBe("high");
    expect(assess({ tx: withValue, owner: own, knownPrograms: new Set([u]) }).level).toBe("low");
  });

  it("flags durable-nonce transactions from dapps", () => {
    const tx = parse([F.transfer(F.friend, 0.1)]);
    tx.usesDurableNonce = true;
    expect(assess({ tx, owner: own, origin: "https://evil.example" }).level).toBe("high");
  });

  it("notes untrusted origins only when value moves", () => {
    const big = assess({ tx: parse([F.transfer(F.stranger, 2)]), owner: own, origin: "https://random.site", originAllowlist: ["https://pump.fun"] });
    expect(big.reasons.some((r) => r.includes("not on your trusted sites"))).toBe(true);
    const small = assess({ tx: parse([F.transfer(F.stranger, 0.01)]), owner: own, origin: "https://random.site", originAllowlist: ["https://pump.fun"] });
    expect(small.level).toBe("low");
  });
});

describe("originAllowed", () => {
  it("matches the whole origin, never a prefix", () => {
    const allow = ["https://pump.fun", "https://jup.ag"];
    expect(originAllowed("https://pump.fun", allow)).toBe(true);
    expect(originAllowed("https://pump.fun/", allow)).toBe(true);
    expect(originAllowed("https://pump.fun.evil.com", allow)).toBe(false);
    expect(originAllowed("https://pump.fun:8443", allow)).toBe(false);
    expect(originAllowed("http://pump.fun", allow)).toBe(false);
    expect(originAllowed("not a url", allow)).toBe(false);
  });
});
