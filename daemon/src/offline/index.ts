/**
 * Offline payments. The robot signs a durable-nonce transaction while it has no network; the signed bytes
 * are kept (and shown as a QR) until the network is back or until the owner carries them out on a phone.
 * A durable nonce keeps the signature valid indefinitely, so nothing expires in the meantime.
 */
import { Connection, NONCE_ACCOUNT_LENGTH, NonceAccount, PublicKey, SystemProgram, Transaction, VersionedTransaction } from "@solana/web3.js";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { randomBytes } from "node:crypto";
import QRCode from "qrcode";
import type { Signer } from "../sign.js";

export interface OfflineEntry { id: string; signedTx: string; summary: string; signedAt: number; usesDurableNonce: boolean; broadcastSig?: string }

export class Offline {
  private entries: OfflineEntry[] = [];
  private path: string;
  constructor(dataDir: string, private signer: Signer, private conn: Connection) {
    mkdirSync(dataDir, { recursive: true });
    this.path = join(dataDir, "offline.json");
    try { if (existsSync(this.path)) this.entries = JSON.parse(readFileSync(this.path, "utf8")); } catch { this.entries = []; }
  }
  private save() { writeFileSync(this.path, JSON.stringify(this.entries, null, 2), { mode: 0o600 }); }

  pending(): OfflineEntry[] { return this.entries.filter((e) => !e.broadcastSig); }

  /** Sign now, keep for later. Presence and touch rules are the normal ones. */
  async sign(txBytes: Uint8Array, origin?: string) {
    const r = await this.signer.sign({ txBytes, origin, source: "api" });
    if (!r.ok) return r;
    const signedTx = Buffer.from(r.signedTx).toString("base64");
    const entry: OfflineEntry = { id: randomBytes(6).toString("hex"), signedTx, summary: r.explanation.spoken, signedAt: Date.now(), usesDurableNonce: r.parsed.usesDurableNonce };
    this.entries.unshift(entry); this.save();
    const qrSvg = await QRCode.toString(signedTx, { type: "svg", errorCorrectionLevel: "L", margin: 1 });
    return { ok: true as const, id: entry.id, signedTx, qrSvg, usesDurableNonce: entry.usesDurableNonce, summary: entry.summary };
  }

  async broadcast(id: string): Promise<{ ok: true; signature: string } | { ok: false; error: string }> {
    const e = this.entries.find((x) => x.id === id);
    if (!e) return { ok: false, error: "not_found" };
    if (e.broadcastSig) return { ok: true, signature: e.broadcastSig };
    try {
      const sig = await this.conn.sendRawTransaction(Buffer.from(e.signedTx, "base64"), { skipPreflight: false, maxRetries: 3 });
      e.broadcastSig = sig; this.save();
      return { ok: true, signature: sig };
    } catch (err) { return { ok: false, error: String((err as Error).message ?? err).slice(0, 200) }; }
  }

  /** Try everything pending; called when the robot reports the network is back. */
  async flush(): Promise<{ id: string; signature?: string; error?: string }[]> {
    const out = [];
    for (const e of this.pending()) { const r = await this.broadcast(e.id); out.push(r.ok ? { id: e.id, signature: r.signature } : { id: e.id, error: r.error }); }
    return out;
  }
}

/** Build the instructions for a brand-new durable-nonce account owned by the owner (one online step). */
export async function nonceAccountCreateTx(conn: Connection, owner: PublicKey): Promise<{ tx: Uint8Array; nonceAccount: string; nonceSecret: Uint8Array }> {
  const { Keypair } = await import("@solana/web3.js");
  const kp = Keypair.generate();
  const rent = await conn.getMinimumBalanceForRentExemption(NONCE_ACCOUNT_LENGTH);
  const tx = new Transaction().add(
    SystemProgram.createAccount({ fromPubkey: owner, newAccountPubkey: kp.publicKey, lamports: rent, space: NONCE_ACCOUNT_LENGTH, programId: SystemProgram.programId }),
    SystemProgram.nonceInitialize({ noncePubkey: kp.publicKey, authorizedPubkey: owner }),
  );
  tx.feePayer = owner;
  tx.recentBlockhash = (await conn.getLatestBlockhash("confirmed")).blockhash;
  tx.partialSign(kp);
  return { tx: new Uint8Array(tx.serialize({ requireAllSignatures: false, verifySignatures: false })), nonceAccount: kp.publicKey.toBase58(), nonceSecret: kp.secretKey };
}

/** Current nonce value of a durable-nonce account, for building offline transactions against it. */
export async function readNonce(conn: Connection, nonceAccount: string): Promise<string> {
  const info = await conn.getAccountInfo(new PublicKey(nonceAccount), "confirmed");
  if (!info) throw new Error("nonce account not found");
  return NonceAccount.fromAccountData(info.data).nonce;
}

export { VersionedTransaction };
