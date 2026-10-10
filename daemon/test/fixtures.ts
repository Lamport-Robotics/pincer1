/** Builds real serialized Solana transactions for the tests, with web3.js, so the parser sees real bytes. */
import { Keypair, PublicKey, SystemProgram, Transaction, TransactionInstruction, TransactionMessage, VersionedTransaction, AddressLookupTableAccount } from "@solana/web3.js";
import { createTransferCheckedInstruction, createApproveInstruction, createSetAuthorityInstruction, createCloseAccountInstruction, AuthorityType, TOKEN_PROGRAM_ID, createTransferInstruction } from "@solana/spl-token";

export const BLOCKHASH = "9hH1wzXmQ6tFJVYxWmH1yh1MYTzF9HbVnR6JvQnfZz1x";
export const owner = Keypair.fromSeed(new Uint8Array(32).fill(7));
export const stranger = Keypair.fromSeed(new Uint8Array(32).fill(9)).publicKey;
export const friend = Keypair.fromSeed(new Uint8Array(32).fill(11)).publicKey;
export const usdc = new PublicKey("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v");
export const ownerUsdc = Keypair.fromSeed(new Uint8Array(32).fill(21)).publicKey; // pretend ATA
export const strangerUsdc = Keypair.fromSeed(new Uint8Array(32).fill(22)).publicKey;

export function legacy(ixs: TransactionInstruction[], feePayer = owner.publicKey): Uint8Array {
  const tx = new Transaction({ feePayer, recentBlockhash: BLOCKHASH }).add(...ixs);
  return new Uint8Array(tx.serialize({ requireAllSignatures: false, verifySignatures: false }));
}

export function v0(ixs: TransactionInstruction[], tables: AddressLookupTableAccount[] = [], feePayer = owner.publicKey): Uint8Array {
  const msg = new TransactionMessage({ payerKey: feePayer, recentBlockhash: BLOCKHASH, instructions: ixs }).compileToV0Message(tables);
  return new VersionedTransaction(msg).serialize();
}

export const transfer = (to: PublicKey, sol: number) => SystemProgram.transfer({ fromPubkey: owner.publicKey, toPubkey: to, lamports: Math.round(sol * 1e9) });
export const usdcTransferChecked = (toAta: PublicKey, amount: number) => createTransferCheckedInstruction(ownerUsdc, usdc, toAta, owner.publicKey, BigInt(Math.round(amount * 1e6)), 6);
export const usdcTransfer = (toAta: PublicKey, amount: number) => createTransferInstruction(ownerUsdc, toAta, owner.publicKey, BigInt(Math.round(amount * 1e6)));
export const approveUnlimited = (delegate: PublicKey) => createApproveInstruction(ownerUsdc, delegate, owner.publicKey, 18446744073709551615n);
export const setOwnerAuthority = (to: PublicKey) => createSetAuthorityInstruction(ownerUsdc, owner.publicKey, AuthorityType.AccountOwner, to);
export const closeToStranger = (to: PublicKey) => createCloseAccountInstruction(ownerUsdc, to, owner.publicKey);
export const unknownProgram = new PublicKey("Fg6PaFpoGXkYsidMpWTK6W2BeZ7FEfcYkg476zPFsLnS");
export const callUnknown = () => new TransactionInstruction({ programId: unknownProgram, keys: [{ pubkey: owner.publicKey, isSigner: true, isWritable: true }], data: Buffer.from([1, 2, 3]) });
export const jupiterLike = () => new TransactionInstruction({ programId: new PublicKey("JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"), keys: [{ pubkey: owner.publicKey, isSigner: true, isWritable: true }], data: Buffer.from([0xe4, 0x45, 0xa5, 0x2e, 0x51, 0xcb, 0x9a, 0x1d]) });
export { TOKEN_PROGRAM_ID, PublicKey, AddressLookupTableAccount };

/** Message bytes (what the controller signs) from a serialized tx. */
export function messageOf(txBytes: Uint8Array): Uint8Array {
  return VersionedTransaction.deserialize(txBytes).message.serialize();
}
