/**
 * Dead-man's switch. The daemon only builds the transaction, asks the controller to sign-and-keep it,
 * relays the controller's warnings to the robot and a webhook, and broadcasts when the controller says
 * the timer expired. The timer itself and the signed bytes live on the controller, so a compromised
 * daemon can neither release early nor cancel.
 */
import { Connection, PublicKey, SystemProgram, Transaction, TransactionInstruction } from "@solana/web3.js";
import { createAssociatedTokenAccountIdempotentInstruction, createTransferCheckedInstruction, getAssociatedTokenAddressSync, TOKEN_PROGRAM_ID } from "@solana/spl-token";
import type { ControllerClient, InheritanceStatus } from "../controller/types.js";
import type { RobotLink } from "../presence/client.js";
import { claimsFor, parseMessage } from "../tx/parse.js";
import { readNonce } from "../offline/index.js";
import { log } from "../config.js";

export interface ArmRequest {
  heir: string;
  inactivityDays: number;
  warnDays: number[];
  /** Fixed SOL amount to pass on (lamports). Default: the current balance minus 0.01 SOL for fees/rent. */
  solLamports?: bigint;
  /** SPL mints to pass on in full (balance at arming time). */
  mints?: string[];
  /** The dedicated durable-nonce account (never used for anything else). */
  nonceAccount: string;
}

export class Inheritance {
  constructor(private controller: ControllerClient, private robot: RobotLink, private conn: Connection, private getOwner: () => string, private webhook = "") {
    controller.onEvent((e) => { if (e.ev === "inheritance_warning") void this.warn(e.days_left ?? 0); });
  }

  private async warn(daysLeft: number) {
    const msg = daysLeft <= 1
      ? "I haven't felt your touch in a long time. If I don't hear from you by tomorrow, I'll pass your wallet to your heir, as you asked. Touch me if you're here."
      : `It's been a while since you touched me. ${daysLeft} days until I pass your wallet to your heir. Touch me to reset.`;
    await this.robot.say(msg);
    if (this.webhook) await fetch(this.webhook, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ event: "inheritance_warning", daysLeft, at: Date.now() }) }).catch(() => {});
    log.warn("inheritance warning", { daysLeft });
  }

  /** Build the durable-nonce transaction the controller will keep. */
  async build(req: ArmRequest): Promise<{ messageBytes: Uint8Array; claims: ReturnType<typeof claimsFor>; summary: string }> {
    const owner = new PublicKey(this.getOwner());
    const heir = new PublicKey(req.heir);
    const nonce = await readNonce(this.conn, req.nonceAccount);
    const ixs: TransactionInstruction[] = [SystemProgram.nonceAdvance({ noncePubkey: new PublicKey(req.nonceAccount), authorizedPubkey: owner })];
    let lamports = req.solLamports;
    if (lamports == null) {
      const bal = await this.conn.getBalance(owner, "confirmed");
      lamports = BigInt(Math.max(0, bal - 10_000_000));
    }
    if (lamports > 0n) ixs.push(SystemProgram.transfer({ fromPubkey: owner, toPubkey: heir, lamports }));
    const tokenLines: string[] = [];
    for (const m of req.mints ?? []) {
      const mint = new PublicKey(m);
      const from = getAssociatedTokenAddressSync(mint, owner);
      const to = getAssociatedTokenAddressSync(mint, heir);
      const acc = await this.conn.getTokenAccountBalance(from, "confirmed").catch(() => null);
      if (!acc || !acc.value.amount || acc.value.amount === "0") continue;
      ixs.push(createAssociatedTokenAccountIdempotentInstruction(owner, to, heir, mint, TOKEN_PROGRAM_ID));
      ixs.push(createTransferCheckedInstruction(from, mint, to, owner, BigInt(acc.value.amount), acc.value.decimals, [], TOKEN_PROGRAM_ID));
      tokenLines.push(`${acc.value.uiAmountString} of ${m.slice(0, 4)}…${m.slice(-4)}`);
    }
    const tx = new Transaction({ feePayer: owner, nonceInfo: { nonce, nonceInstruction: ixs[0] } }).add(...ixs.slice(1));
    const messageBytes = new Uint8Array(tx.compileMessage().serialize());
    const parsed = parseMessage(messageBytes, { owner: owner.toBase58() });
    const summary = `${Number(lamports) / 1e9} SOL${tokenLines.length ? " and " + tokenLines.join(", ") : ""} to ${req.heir.slice(0, 4)}…${req.heir.slice(-4)} after ${req.inactivityDays} days without a touch`;
    return { messageBytes, claims: claimsFor(parsed), summary };
  }

  async arm(req: ArmRequest) {
    const b = await this.build(req);
    await this.robot.say(`Inheritance: ${b.summary}. Hold your hand on me for three seconds to arm it.`);
    const r = await this.controller.inheritanceSet(b.messageBytes, b.claims, { inactivityDays: req.inactivityDays, warnDays: req.warnDays, heir: req.heir });
    await this.robot.say(r.ok ? "Armed. Touch me now and then, and nothing happens." : "Not armed.");
    return r.ok ? { ok: true as const, summary: b.summary } : { ok: false as const, reason: r.reason };
  }

  status(): Promise<InheritanceStatus> { return this.controller.inheritanceStatus(); }

  async clear() { const ok = await this.controller.inheritanceClear(); await this.robot.say(ok ? "Inheritance cleared." : "Not cleared."); return ok; }

  /** Ask the controller for the stored transaction; broadcast only if it says the timer expired. */
  async release(): Promise<{ ok: true; signature: string } | { ok: false; reason: string }> {
    const r = await this.controller.inheritanceRelease();
    if (!r.ok) return { ok: false, reason: r.reason };
    try {
      await this.robot.network(true, "broadcast", 300);
      const sig = await this.conn.sendRawTransaction(r.tx, { skipPreflight: false, maxRetries: 5 });
      await this.robot.say("I've passed the wallet to your heir, as you asked.");
      if (this.webhook) await fetch(this.webhook, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ event: "inheritance_released", signature: sig, at: Date.now() }) }).catch(() => {});
      return { ok: true, signature: sig };
    } catch (e) { return { ok: false, reason: String((e as Error).message ?? e).slice(0, 200) }; }
  }

  /** Periodic: if the timer has expired, release; otherwise nothing. The daemon calls this hourly. */
  async tick() {
    const s = await this.status().catch(() => null);
    if (!s?.armed) return null;
    if (s.days_since_touch >= s.inactivity_days) return this.release();
    return null;
  }
}
