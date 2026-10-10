/**
 * Transaction parser. Turns a serialized Solana message (legacy or v0) into a flat, owner-centric view:
 * what leaves the owner's wallet, which programs run, and which instructions are dangerous.
 *
 * This mirrors what the consent controller computes on-device (firmware/src/solmsg.c). The daemon's result
 * is what gets *explained* to the owner; the controller's result is what gets *enforced*. The two must
 * agree or the controller denies with `claims_mismatch`.
 */
import { PublicKey, VersionedMessage, type MessageV0, type Message } from "@solana/web3.js";
import { ATA, COMPUTE_BUDGET, LOOKUP_TABLE, SYSTEM, TOKEN, TOKEN_2022, U64_MAX } from "./known.js";

export interface TokenMove {
  mint: string | null; // null when the instruction does not carry the mint (plain `transfer`)
  amount: bigint; // raw units
  source: string; // token account
  dest: string; // token account or wallet (for closeAccount: lamport destination)
}

export type DangerKind = "set_authority" | "approve" | "close_account" | "assign" | "lookup_table" | "nonce" | "burn";

export interface Danger {
  kind: DangerKind;
  program: string;
  detail: string;
  /** The key that receives the power (new authority, delegate, lamport destination). */
  to?: string;
  amount?: bigint;
}

export interface ParsedInstruction {
  index: number;
  program: string;
  accounts: string[];
  data: Uint8Array;
  /** Human label when the instruction is recognised (System transfer, Token transferChecked, …). */
  label?: string;
}

export interface ParsedTx {
  version: "legacy" | 0;
  feePayer: string;
  signers: string[];
  accounts: string[]; // static keys (+ resolved lookups for v0 when provided)
  unresolvedLookups: boolean; // v0 with lookup tables that were not resolved
  instructions: ParsedInstruction[];
  programs: string[];
  /** SOL leaving the owner via System instructions, in lamports. */
  lamportsOut: bigint;
  /** Primary recipient of SOL: the destination receiving the most lamports from the owner. */
  dest: string | null;
  tokenOut: TokenMove[];
  tokenIn: TokenMove[];
  dangers: Danger[];
  usesDurableNonce: boolean;
  memo: string | null;
}

const u64 = (d: Uint8Array, o: number) => (o + 8 <= d.length ? new DataView(d.buffer, d.byteOffset + o, 8).getBigUint64(0, true) : 0n);
const u32 = (d: Uint8Array, o: number) => (o + 4 <= d.length ? new DataView(d.buffer, d.byteOffset + o, 4).getUint32(0, true) : 0xffffffff);

export interface ParseOptions {
  /** The owner whose perspective we take. Defaults to the fee payer. */
  owner?: string;
  /** Resolved address-lookup-table contents for v0 messages: table pubkey → addresses. */
  lookupTables?: Record<string, string[]>;
}

/** True when the bytes deserialize as a Solana message (used to refuse disguised `signMessage` payloads). */
export function looksLikeMessage(bytes: Uint8Array): boolean {
  try {
    const m = VersionedMessage.deserialize(bytes);
    return m.staticAccountKeys.length > 0 && m.compiledInstructions.length > 0;
  } catch {
    return false;
  }
}

export function parseMessage(bytes: Uint8Array, opts: ParseOptions = {}): ParsedTx {
  const msg = VersionedMessage.deserialize(bytes);
  const version = msg.version;
  const staticKeys = msg.staticAccountKeys.map((k) => k.toBase58());
  let accounts = [...staticKeys];
  let unresolvedLookups = false;

  if (version === 0) {
    const v0 = msg as MessageV0;
    const writable: string[] = [];
    const readonly: string[] = [];
    for (const l of v0.addressTableLookups) {
      const table = opts.lookupTables?.[l.accountKey.toBase58()];
      if (!table) {
        unresolvedLookups = true;
        writable.push(...l.writableIndexes.map(() => "?"));
        readonly.push(...l.readonlyIndexes.map(() => "?"));
      } else {
        writable.push(...l.writableIndexes.map((i) => table[i] ?? "?"));
        readonly.push(...l.readonlyIndexes.map((i) => table[i] ?? "?"));
      }
    }
    accounts = [...staticKeys, ...writable, ...readonly];
  }

  const header = msg.header;
  const feePayer = staticKeys[0];
  const owner = opts.owner ?? feePayer;
  const signers = staticKeys.slice(0, header.numRequiredSignatures);

  const instructions: ParsedInstruction[] = msg.compiledInstructions.map((ci, index) => ({
    index,
    program: accounts[ci.programIdIndex] ?? "?",
    accounts: ci.accountKeyIndexes.map((i) => accounts[i] ?? "?"),
    data: ci.data,
  }));

  const out: ParsedTx = {
    version,
    feePayer,
    signers,
    accounts,
    unresolvedLookups,
    instructions,
    programs: [...new Set(instructions.map((i) => i.program))],
    lamportsOut: 0n,
    dest: null,
    tokenOut: [],
    tokenIn: [],
    dangers: [],
    usesDurableNonce: false,
    memo: null,
  };

  const lamportsTo = new Map<string, bigint>();
  const addLamports = (to: string, n: bigint) => {
    out.lamportsOut += n;
    lamportsTo.set(to, (lamportsTo.get(to) ?? 0n) + n);
  };

  for (const ix of instructions) {
    const d = ix.data;
    if (ix.program === SYSTEM) {
      const kind = u32(d, 0);
      const from = ix.accounts[0];
      switch (kind) {
        case 0: // createAccount: [from, new] lamports u64 @4
          ix.label = "System.createAccount";
          if (from === owner) addLamports(ix.accounts[1], u64(d, 4));
          if (ix.accounts[1] === owner) out.dangers.push({ kind: "assign", program: SYSTEM, detail: "re-creates the owner account under another program" });
          break;
        case 1: // assign: [account] owner pubkey @4
          ix.label = "System.assign";
          if (from === owner) out.dangers.push({ kind: "assign", program: SYSTEM, detail: "assigns your wallet account to another program", to: new PublicKey(d.subarray(4, 36)).toBase58() });
          break;
        case 2: // transfer: [from, to] lamports @4
          ix.label = "System.transfer";
          if (from === owner) addLamports(ix.accounts[1], u64(d, 4));
          break;
        case 3: { // createAccountWithSeed: [from, new, base?] base(32) seedLen(u64) seed lamports...
          ix.label = "System.createAccountWithSeed";
          const seedLen = Number(u64(d, 36));
          const lamports = u64(d, 44 + seedLen);
          if (from === owner) addLamports(ix.accounts[1], lamports);
          break;
        }
        case 4: // advanceNonceAccount
          ix.label = "System.advanceNonce";
          out.usesDurableNonce = true;
          break;
        case 8: // allocate
          ix.label = "System.allocate";
          break;
        case 11: { // transferWithSeed: [from, base, to] lamports @4
          ix.label = "System.transferWithSeed";
          if (ix.accounts[1] === owner) addLamports(ix.accounts[2], u64(d, 4));
          break;
        }
        default:
          ix.label = `System.${kind}`;
      }
      continue;
    }

    if (ix.program === TOKEN || ix.program === TOKEN_2022) {
      const kind = d[0];
      const prog = ix.program;
      switch (kind) {
        case 3: { // transfer: [source, dest, authority] amount u64 @1
          ix.label = "Token.transfer";
          const mv: TokenMove = { mint: null, amount: u64(d, 1), source: ix.accounts[0], dest: ix.accounts[1] };
          if (ix.accounts[2] === owner) out.tokenOut.push(mv); else out.tokenIn.push(mv);
          break;
        }
        case 12: { // transferChecked: [source, mint, dest, authority] amount u64 @1, decimals u8 @9
          ix.label = "Token.transferChecked";
          const mv: TokenMove = { mint: ix.accounts[1], amount: u64(d, 1), source: ix.accounts[0], dest: ix.accounts[2] };
          if (ix.accounts[3] === owner) out.tokenOut.push(mv); else out.tokenIn.push(mv);
          break;
        }
        case 4: // approve: [source, delegate, owner] amount @1
        case 13: { // approveChecked: [source, mint, delegate, owner] amount @1
          ix.label = kind === 4 ? "Token.approve" : "Token.approveChecked";
          const ownerIdx = kind === 4 ? 2 : 3;
          const delegate = ix.accounts[kind === 4 ? 1 : 2];
          if (ix.accounts[ownerIdx] === owner) {
            const amount = u64(d, 1);
            out.dangers.push({ kind: "approve", program: prog, detail: amount === U64_MAX ? "unlimited token delegation" : "token delegation", to: delegate, amount });
          }
          break;
        }
        case 6: { // setAuthority: [account, currentAuthority] type u8 @1, option u8 @2, newAuthority @3
          ix.label = "Token.setAuthority";
          if (ix.accounts[1] === owner) {
            const type = d[1];
            const has = d[2] === 1;
            const to = has ? new PublicKey(d.subarray(3, 35)).toBase58() : undefined;
            const what = ["mint tokens", "freeze account", "account owner", "close account"][type] ?? `authority ${type}`;
            out.dangers.push({ kind: "set_authority", program: prog, detail: `hands over the "${what}" authority of ${ix.accounts[0]}`, to });
          }
          break;
        }
        case 9: { // closeAccount: [account, dest, owner]
          ix.label = "Token.closeAccount";
          if (ix.accounts[2] === owner && ix.accounts[1] !== owner) {
            out.dangers.push({ kind: "close_account", program: prog, detail: `closes ${ix.accounts[0]} and sends its rent to ${ix.accounts[1]}`, to: ix.accounts[1] });
          }
          break;
        }
        case 8: { // burn: [account, mint, owner] amount @1
          ix.label = "Token.burn";
          if (ix.accounts[2] === owner) out.dangers.push({ kind: "burn", program: prog, detail: `burns ${u64(d, 1)} units of ${ix.accounts[1]}`, amount: u64(d, 1) });
          break;
        }
        case 1: ix.label = "Token.initializeAccount"; break;
        case 18: ix.label = "Token.initializeAccount3"; break;
        case 17: ix.label = "Token.syncNative"; break;
        default: ix.label = `Token.${kind}`;
      }
      continue;
    }

    if (ix.program === ATA) ix.label = "ATA.create";
    else if (ix.program === COMPUTE_BUDGET) ix.label = "ComputeBudget";
    else if (ix.program === LOOKUP_TABLE) {
      ix.label = "LookupTable";
      out.dangers.push({ kind: "lookup_table", program: LOOKUP_TABLE, detail: "modifies an address lookup table" });
    } else if (ix.program === "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr" || ix.program === "Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo") {
      ix.label = "Memo";
      out.memo = new TextDecoder().decode(d).slice(0, 200);
    }
  }

  if (unresolvedLookups) out.dangers.push({ kind: "lookup_table", program: LOOKUP_TABLE, detail: "uses address lookup tables that could not be resolved" });

  let best: [string, bigint] | null = null;
  for (const e of lamportsTo) if (!best || e[1] > best[1]) best = e;
  out.dest = best?.[0] ?? null;
  return out;
}

/** The `claims` object the daemon sends to the controller alongside the bytes. */
export function claimsFor(p: ParsedTx) {
  return {
    lamports_out: p.lamportsOut.toString(),
    token_out: p.tokenOut.map((t) => ({ mint: t.mint ?? "", amount: t.amount.toString(), dest: t.dest ?? "" })),
    dest: p.dest ?? "",
    programs: p.programs,
    dangerous: p.dangers.map((d) => d.kind),
  };
}

export type { Message, MessageV0 };
