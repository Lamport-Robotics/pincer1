/**
 * Transaction parser. Turns a serialized Solana message (legacy or v0) into a flat, owner-centric view:
 * what leaves the owner's wallet, which programs run, and which instructions are dangerous.
 *
 * This mirrors what the consent controller computes on-device (firmware/src/solmsg.c). The daemon's result
 * is what gets *explained* to the owner; the controller's result is what gets *enforced*. The two must
 * agree or the controller denies with `claims_mismatch`.
 */
import { PublicKey, VersionedMessage, VersionedTransaction, type MessageV0, type Message } from "@solana/web3.js";
import { ATA, COMPUTE_BUDGET, JUPITER_V6, LOOKUP_TABLE, MEMO, SYSTEM, TOKEN, TOKEN_2022, U64_MAX } from "./known.js";

export interface TokenMove {
  mint: string | null; // null when the instruction does not carry the mint (plain `transfer`)
  amount: bigint; // raw units
  source: string; // token account
  dest: string; // token account or wallet (for closeAccount: lamport destination); "" for a burn
}

export type DangerKind = "set_authority" | "approve" | "close_account" | "assign" | "lookup_table" | "unknown_instruction" | "nonce" | "burn";

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
  version: "legacy" | 0 | 1;
  feePayer: string;
  signers: string[];
  accounts: string[]; // static keys (+ resolved lookups for v0 when provided)
  unresolvedLookups: boolean; // v0 with lookup tables that were not resolved
  touchesLookups: boolean; // an instruction's program or accounts come from a lookup table: opaque to the controller
  instructions: ParsedInstruction[];
  programs: string[];
  /** SOL leaving the owner via System instructions, in lamports. */
  lamportsOut: bigint;
  /** What the fee payer pays the leader: 5000 lamports per signature plus the ComputeBudget priority fee. Mirrors solmsg.c. */
  feeLamports: bigint;
  /** The ComputeBudget part alone (unit limit x unit price, rounded up). Counted against limits like a transfer. */
  priorityLamports: bigint;
  /** Primary recipient of SOL: the destination receiving the most lamports from the owner. */
  dest: string | null;
  /** Distinct destinations that receive lamports from the owner; the controller shows one and refuses more. */
  solRecipients: number;
  tokenOut: TokenMove[];
  tokenIn: TokenMove[];
  dangers: Danger[];
  usesDurableNonce: boolean;
  memo: string | null;
}

/** Token / Token-2022 tags that move nothing out of the owner (mirrors the list in firmware solmsg.c). */
const TOKEN_BENIGN = new Set([0, 1, 2, 5, 7, 10, 11, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31, 32, 33, 34, 36, 39, 40, 41]);
const BASE_FEE_PER_SIG = 5000n;
const MAX_CU = 1_400_000n;
const DEFAULT_CU_PER_IX = 200_000n;

const u64 = (d: Uint8Array, o: number) => (o + 8 <= d.length ? new DataView(d.buffer, d.byteOffset + o, 8).getBigUint64(0, true) : 0n);
const u32 = (d: Uint8Array, o: number) => (o + 4 <= d.length ? new DataView(d.buffer, d.byteOffset + o, 4).getUint32(0, true) : 0xffffffff);

export interface ParseOptions {
  /** The owner whose perspective we take. Defaults to the fee payer. */
  owner?: string;
  /** Token accounts known to belong to the owner (for plain Token.transfer, which names no mint). */
  ownerTokenAccounts?: Set<string>;
  /** Resolved address-lookup-table contents for v0 messages: table pubkey → addresses. */
  lookupTables?: Record<string, string[]>;
}

const RING_DERIVE_TAG = new TextEncoder().encode("TSPP/derive/");
/** SHA-256("TSPP/derive/v1"): the application domain of the off-chain envelope the keys are derived from. */
const RING_DERIVE_DOMAIN_V1 = Uint8Array.from(Buffer.from("1d32a88533af12d35e5ac6fce817a4cb810bcc4115386b14a78e8b2ef09d864c", "hex"));
function contains(bytes: Uint8Array, needle: Uint8Array): boolean {
  const n = needle.length;
  outer: for (let i = 0; i + n <= bytes.length; i++) {
    for (let j = 0; j < n; j++) if (bytes[i + j] !== needle[j]) continue outer;
    return true;
  }
  return false;
}
/**
 * True when the bytes are, or contain, a Privacy Rings key-derivation message: the text `TSPP/derive/` of any
 * version, raw or inside any off-chain envelope, or the application domain the canonical envelope carries.
 * The signature over it is deterministic and every privacy key of the
 * wallet is derived from it, so `signMessage` must never produce it. The controller refuses the same bytes.
 */
export function isRingKeyDerivation(bytes: Uint8Array): boolean {
  return contains(bytes, RING_DERIVE_TAG) || contains(bytes, RING_DERIVE_DOMAIN_V1);
}

/** The longest message the secure element signs in one command (firmware: SIGNER_ED25519_MSG_MAX; provisional). */
export const ED25519_MSG_MAX = 860;
/** Privacy Rings pool and registry. Until the controller parses their instructions, it refuses any transaction naming them. */
export const PRIVACY_RINGS_PROGRAMS: readonly string[] = ["ring1GaLCuJnkUMVkX2JeZLTTX17wKsV4fZmPVzXANW", "regy3WAeRXMYuMYxr5iK3Bf9Arr3PcxGxyzAddbiqDQ"];
const OFFCHAIN_DOMAIN = Uint8Array.from([0xff, ...Buffer.from("solana offchain")]);
const NS_TSPP = Uint8Array.from(Buffer.from("TSPP/"));
const NS_ZOLANA = Uint8Array.from(Buffer.from("zolana/"));
const SIGN_IN_TAIL = " wants you to sign in with your Solana account:";

function isText(b: Uint8Array, allowNewline: boolean): boolean {
  for (const c of b) {
    if (c === 0x0a) { if (!allowNewline) return false; continue; }
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}
function startsWith(b: Uint8Array, prefix: Uint8Array): boolean {
  if (b.length < prefix.length) return false;
  for (let i = 0; i < prefix.length; i++) if (b[i] !== prefix[i]) return false;
  return true;
}
function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && startsWith(a, b);
}

/**
 * The bare-text rule of the controller (firmware: solmsg_bare_text_ok): every byte 0x20..0x7e or a line feed,
 * not exactly 32 bytes, and with a space once it is 32 bytes or longer. A digest, a hex, base58 or base64
 * string and compact JSON all fail it. Also used for EIP-191 and BIP-322 messages.
 */
export function bareTextOk(bytes: Uint8Array): boolean {
  if (bytes.length === 0 || bytes.length === 32) return false;
  if (!isText(bytes, true)) return false;
  return bytes.length < 32 || bytes.includes(0x20);
}

export type SignMessageCheck =
  | { ok: true; shape: "text" | "offchain_v0" | "offchain_v1"; text: Uint8Array; appDomain?: Uint8Array }
  | { ok: false };

/**
 * What `signMessage` may sign, as the controller decides it (firmware: solmsg_signmsg_check; shared vectors in
 * vectors/solana_msg). Bare text, or a Solana off-chain message (header version 0 or 1) with exactly one
 * signer, the wallet's own key, and a text body. Refused: any other payload starting at 0x80 or above (every
 * versioned transaction message, version 1 included), the Privacy Rings key namespaces, a wallet key outside
 * the envelope's signer field, a sign-in text naming another account.
 * `ownKey` null skips the checks that need the key (used before the controller is asked, where only "can this
 * be read at all" matters).
 */
export function checkSignMessage(bytes: Uint8Array, ownKey: Uint8Array | null, otherKey?: Uint8Array | null): SignMessageCheck {
  const no: SignMessageCheck = { ok: false };
  if (bytes.length === 0) return no;
  if (isRingKeyDerivation(bytes) || contains(bytes, NS_TSPP) || contains(bytes, NS_ZOLANA)) return no;
  let shape: "text" | "offchain_v0" | "offchain_v1" = "text";
  let text = bytes;
  let appDomain: Uint8Array | undefined;
  let signerAt = -1;
  if (startsWith(bytes, OFFCHAIN_DOMAIN)) {
    if (bytes.length < 18) return no;
    if (bytes[16] === 0) {
      if (bytes.length <= 85 || bytes.length > 1232) return no;
      if (bytes[49]! > 1 || bytes[50] !== 1) return no;
      signerAt = 51;
      const bodyLen = bytes[83]! | (bytes[84]! << 8);
      if (bodyLen === 0 || bodyLen !== bytes.length - 85) return no;
      shape = "offchain_v0";
      text = bytes.subarray(85);
      const app = bytes.subarray(17, 49);
      if (app.some((b) => b !== 0)) appDomain = app;
      if (!isText(text, bytes[49] === 1)) return no;
    } else if (bytes[16] === 1) {
      if (bytes.length <= 50 || bytes[17] !== 1) return no;
      signerAt = 18;
      shape = "offchain_v1";
      text = bytes.subarray(50);
      if (!isText(text, true)) return no;
    } else return no;
    if (ownKey && !sameBytes(bytes.subarray(signerAt, signerAt + 32), ownKey)) return no;
  } else {
    if (bytes[0]! >= 0x80) return no;
    if (!bareTextOk(bytes)) return no;
  }
  for (let i = 0; i + 32 <= bytes.length; i++) {
    if (i === signerAt) continue;
    const w = bytes.subarray(i, i + 32);
    if ((ownKey && sameBytes(w, ownKey)) || (otherKey && sameBytes(w, otherKey))) return no;
  }
  if (ownKey) {
    const s = Buffer.from(text).toString("latin1");
    const eol = s.indexOf("\n");
    const first = eol < 0 ? s : s.slice(0, eol);
    if (first.endsWith(SIGN_IN_TAIL)) {
      if (eol < 0) return no;
      const e2 = s.indexOf("\n", eol + 1);
      const second = e2 < 0 ? s.slice(eol + 1) : s.slice(eol + 1, e2);
      if (second !== new PublicKey(ownKey).toBase58()) return no;
    }
  }
  return { ok: true, shape, text, ...(appDomain ? { appDomain } : {}) };
}

/** True when a message, in any wire format, names the Privacy Rings pool or registry among its keys (firmware, legacy and v0: solmsg_touches_privacy_rings). */
export function touchesPrivacyRings(bytes: Uint8Array): boolean {
  try {
    const keys = bytes.length > 0 && bytes[0] === 0x81 ? decodeV1Message(bytes).staticKeys : VersionedMessage.deserialize(bytes).staticAccountKeys.map((k) => k.toBase58());
    return keys.some((k) => PRIVACY_RINGS_PROGRAMS.includes(k));
  } catch {
    return false;
  }
}

/** True when the bytes deserialize as a Solana message (used to refuse disguised `signMessage` payloads). */
export function looksLikeMessage(bytes: Uint8Array): boolean {
  if (bytes.length > 0 && bytes[0] === 0x81) { try { decodeV1Message(bytes); return true; } catch { return false; } }
  try {
    const m = VersionedMessage.deserialize(bytes);
    return m.staticAccountKeys.length > 0 && m.compiledInstructions.length > 0;
  } catch {
    return false;
  }
}

/** A message in whichever wire format, reduced to what the reader below needs. */
interface DecodedMessage {
  version: "legacy" | 0 | 1;
  staticKeys: string[];
  numRequiredSignatures: number;
  lookups: { accountKey: string; writableIndexes: number[]; readonlyIndexes: number[] }[];
  instructions: { programIdIndex: number; accountKeyIndexes: number[]; data: Uint8Array }[];
  /** Version 1 carries its fee and limits in the header, not in ComputeBudget instructions. */
  v1?: { priorityLamports: bigint; computeUnitLimit: number | null };
}

function decodeLegacyOrV0(bytes: Uint8Array): DecodedMessage {
  const msg = VersionedMessage.deserialize(bytes);
  return {
    version: msg.version,
    staticKeys: msg.staticAccountKeys.map((k) => k.toBase58()),
    numRequiredSignatures: msg.header.numRequiredSignatures,
    lookups: msg.version === 0 ? (msg as MessageV0).addressTableLookups.map((l) => ({ accountKey: l.accountKey.toBase58(), writableIndexes: [...l.writableIndexes], readonlyIndexes: [...l.readonlyIndexes] })) : [],
    instructions: msg.compiledInstructions.map((ci) => ({ programIdIndex: ci.programIdIndex, accountKeyIndexes: [...ci.accountKeyIndexes], data: ci.data })),
  };
}

/** The largest version 1 transaction (SIMD-0385). A message is that minus at least one signature. */
export const V1_MAX_TRANSACTION_BYTES = 4096;

/**
 * Decodes a Solana version 1 transaction message (SIMD-0385): the bytes that are signed, which on the wire
 * come BEFORE the signatures. Layout: 0x81, the three header bytes, a u32 config mask, the blockhash, the
 * instruction and address counts, the addresses, four bytes per set mask bit, a four-byte header per
 * instruction, then each instruction's account indexes and data. No lookup tables. Every sanitization rule of
 * the proposal is applied, and anything left over is an error: a reader that guesses is worse than none.
 */
export function decodeV1Message(bytes: Uint8Array): DecodedMessage {
  const bad = (why: string): never => { throw new Error(`version 1 message: ${why}`); };
  if (bytes.length > V1_MAX_TRANSACTION_BYTES - 64) bad("too long");
  let o = 0;
  const need = (n: number) => { if (o + n > bytes.length) bad("truncated"); };
  const u8 = () => { need(1); return bytes[o++]!; };
  if (u8() !== 0x81) bad("not version 1");
  const numSigs = u8(), roSigned = u8(), roUnsigned = u8();
  if (numSigs < 1 || numSigs > 12) bad("signature count");
  if (roSigned >= numSigs) bad("read-only signer count");
  need(4);
  const mask = (bytes[o]! | (bytes[o + 1]! << 8) | (bytes[o + 2]! << 16) | (bytes[o + 3]! << 24)) >>> 0;
  o += 4;
  if (mask & ~0x1f) bad("unknown config bits");
  if (((mask & 1) !== 0) !== ((mask & 2) !== 0)) bad("half a priority fee");
  need(32); o += 32; // lifetime specifier (recent blockhash)
  const numIx = u8(), numAddr = u8();
  if (numIx > 64) bad("too many instructions");
  if (numAddr > 64 || numAddr < numSigs + roUnsigned) bad("address count");
  need(32 * numAddr);
  const staticKeys: string[] = [];
  for (let i = 0; i < numAddr; i++) { staticKeys.push(new PublicKey(bytes.subarray(o, o + 32)).toBase58()); o += 32; }
  if (new Set(staticKeys).size !== staticKeys.length) bad("duplicate address");
  const word = () => { need(4); const v = (bytes[o]! | (bytes[o + 1]! << 8) | (bytes[o + 2]! << 16) | (bytes[o + 3]! << 24)) >>> 0; o += 4; return v; };
  let priorityLamports = 0n;
  let computeUnitLimit: number | null = null;
  if (mask & 1) { const lo = word(), hi = word(); priorityLamports = (BigInt(hi) << 32n) | BigInt(lo); }
  if (mask & 4) computeUnitLimit = word();
  if (mask & 8) word(); // loaded accounts data size limit
  if (mask & 16) { const heap = word(); if (heap % 1024 !== 0 || heap < 32 * 1024 || heap > 256 * 1024) bad("heap size"); }
  const headers: { programIdIndex: number; nAcc: number; nData: number }[] = [];
  for (let i = 0; i < numIx; i++) { need(4); headers.push({ programIdIndex: bytes[o]!, nAcc: bytes[o + 1]!, nData: bytes[o + 2]! | (bytes[o + 3]! << 8) }); o += 4; }
  const instructions: DecodedMessage["instructions"] = [];
  for (const h of headers) {
    if (h.programIdIndex >= numAddr) bad("program index");
    need(h.nAcc + h.nData);
    const accountKeyIndexes = [...bytes.subarray(o, o + h.nAcc)]; o += h.nAcc;
    if (accountKeyIndexes.some((i) => i >= numAddr)) bad("account index");
    const data = bytes.subarray(o, o + h.nData); o += h.nData;
    instructions.push({ programIdIndex: h.programIdIndex, accountKeyIndexes, data });
  }
  if (o !== bytes.length) bad("trailing bytes");
  return { version: 1, staticKeys, numRequiredSignatures: numSigs, lookups: [], instructions, v1: { priorityLamports, computeUnitLimit } };
}

/**
 * Reads a serialized Solana message in any wire format the controller parses: legacy, v0 and version 1
 * (firmware: solmsg_parse). Throws on anything else.
 */
export function parseMessage(bytes: Uint8Array, opts: ParseOptions = {}): ParsedTx {
  return readDecoded(bytes.length > 0 && bytes[0] === 0x81 ? decodeV1Message(bytes) : decodeLegacyOrV0(bytes), opts);
}
/** The same function under the name Warden uses. */
export const parseAnyMessage = parseMessage;

/**
 * Splits a serialized transaction into the message that is signed and where each signature goes. Legacy and v0
 * put the signatures first; version 1 puts them after the message.
 */
export function splitTransaction(txBytes: Uint8Array): { messageBytes: Uint8Array; version1: boolean } {
  if (txBytes.length > 0 && txBytes[0] === 0x81) {
    const numSigs = txBytes[1] ?? 0;
    if (numSigs < 1 || txBytes.length <= 64 * numSigs) throw new Error("version 1 transaction: too short for its signatures");
    return { messageBytes: txBytes.subarray(0, txBytes.length - 64 * numSigs), version1: true };
  }
  return { messageBytes: VersionedTransaction.deserialize(txBytes).message.serialize(), version1: false };
}

/** Puts `owner`'s signature into its slot of a version 1 transaction (the i-th signature belongs to the i-th address). */
export function addV1Signature(txBytes: Uint8Array, owner: string, sig: Uint8Array): Uint8Array {
  const { messageBytes } = splitTransaction(txBytes);
  const m = decodeV1Message(messageBytes);
  const i = m.staticKeys.slice(0, m.numRequiredSignatures).indexOf(owner);
  if (i < 0) throw new Error("version 1 transaction: the wallet is not one of its signers");
  if (sig.length !== 64) throw new Error("signature must be 64 bytes");
  const out = Uint8Array.from(txBytes);
  out.set(sig, messageBytes.length + 64 * i);
  return out;
}

function readDecoded(msg: DecodedMessage, opts: ParseOptions): ParsedTx {
  const version = msg.version;
  const staticKeys = msg.staticKeys;
  let accounts = [...staticKeys];
  let unresolvedLookups = false;

  if (version === 0) {
    const writable: string[] = [];
    const readonly: string[] = [];
    for (const l of msg.lookups) {
      const table = opts.lookupTables?.[l.accountKey];
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

  const header = { numRequiredSignatures: msg.numRequiredSignatures };
  const feePayer = staticKeys[0]!;
  const owner = opts.owner ?? feePayer;
  const signers = staticKeys.slice(0, header.numRequiredSignatures);
  // an inflow is only an inflow when it lands in the owner's own account: a transfer into a stranger's token
  // account must never be read out as "you receive"
  const ownerAtas = new Map<string, string>();
  const ownsDest = (dest: string, mint: string | null): boolean => {
    if (dest === owner || opts.ownerTokenAccounts?.has(dest)) return true;
    if (!mint) return false;
    return ownerAtaSet(owner, mint, ownerAtas).has(dest);
  };
  const inflow = (mv: TokenMove) => { if (mv.dest && ownsDest(mv.dest, mv.mint)) out.tokenIn.push(mv); };

  // the controller cannot resolve lookup tables (solmsg.c): any instruction that reaches past the static keys is
  // flagged there, resolved here or not, and must be declared
  const touchesLookups = msg.instructions.some((ci) => ci.programIdIndex >= staticKeys.length || ci.accountKeyIndexes.some((i) => i >= staticKeys.length));
  const instructions: ParsedInstruction[] = msg.instructions.map((ci, index) => ({
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
    touchesLookups,
    instructions,
    programs: [...new Set(instructions.map((i) => i.program))],
    lamportsOut: 0n,
    solRecipients: 0,
    feeLamports: 0n,
    priorityLamports: 0n,
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
        case 5: // withdrawNonceAccount: [nonce, to, blockhashes, rent, authority] lamports @4
          ix.label = "System.withdrawNonce";
          if (ix.accounts[4] === owner) addLamports(ix.accounts[1], u64(d, 4));
          break;
        case 6: // initializeNonceAccount
          ix.label = "System.initializeNonce";
          break;
        case 7: // authorizeNonceAccount: [nonce, authority] new authority @4
          ix.label = "System.authorizeNonce";
          if (ix.accounts[1] === owner) out.dangers.push({ kind: "set_authority", program: SYSTEM, detail: `hands the nonce account ${ix.accounts[0]} to another authority`, to: new PublicKey(d.subarray(4, 36)).toBase58() });
          break;
        case 8: // allocate
          ix.label = "System.allocate";
          break;
        case 9: // allocateWithSeed: [account, base]
        case 10: // assignWithSeed: [account, base]
          ix.label = kind === 9 ? "System.allocateWithSeed" : "System.assignWithSeed";
          if (ix.accounts[1] === owner) out.dangers.push({ kind: "assign", program: SYSTEM, detail: "assigns an account derived from your wallet to another program" });
          break;
        case 12: // upgradeNonceAccount
          ix.label = "System.upgradeNonce";
          break;
        case 11: { // transferWithSeed: [from, base, to] lamports @4
          ix.label = "System.transferWithSeed";
          if (ix.accounts[1] === owner) addLamports(ix.accounts[2], u64(d, 4));
          break;
        }
        default:
          ix.label = `System.${kind}`;
          // the controller refuses System instructions it cannot classify unless they are declared; declare them
          out.dangers.push({ kind: "unknown_instruction", program: SYSTEM, detail: `System instruction ${kind} is not understood` });
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
          if (ix.accounts[2] === owner) out.tokenOut.push(mv); else inflow(mv);
          break;
        }
        case 12: { // transferChecked: [source, mint, dest, authority] amount u64 @1, decimals u8 @9
          ix.label = "Token.transferChecked";
          const mv: TokenMove = { mint: ix.accounts[1], amount: u64(d, 1), source: ix.accounts[0], dest: ix.accounts[2] };
          if (ix.accounts[3] === owner) out.tokenOut.push(mv); else inflow(mv);
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
          if (ix.accounts[2] === owner) { // any close the owner authorises is declared, wherever the rent goes (PROTOCOL §1)
            out.dangers.push({ kind: "close_account", program: prog, detail: `closes ${ix.accounts[0]} and sends its rent to ${ix.accounts[1]}`, to: ix.accounts[1] });
          }
          break;
        }
        case 8: { // burn: [account, mint, owner] amount @1
          ix.label = "Token.burn";
          if (ix.accounts[2] === owner) {
            // the controller counts a burn as a token outflow with no recipient: claim it as one
            out.tokenOut.push({ mint: ix.accounts[1], amount: u64(d, 1), source: ix.accounts[0], dest: "" });
            out.dangers.push({ kind: "burn", program: prog, detail: `burns ${u64(d, 1)} units of ${ix.accounts[1]}`, amount: u64(d, 1) });
          }
          break;
        }
        case 15: { // burnChecked: [account, mint, owner] amount @1, decimals @9
          ix.label = "Token.burnChecked";
          if (ix.accounts[2] === owner) {
            out.tokenOut.push({ mint: ix.accounts[1], amount: u64(d, 1), source: ix.accounts[0], dest: "" });
            out.dangers.push({ kind: "burn", program: prog, detail: `burns ${u64(d, 1)} units of ${ix.accounts[1]}`, amount: u64(d, 1) });
          }
          break;
        }
        case 26: { // Token-2022 TransferFeeExtension; sub 1 = transferCheckedWithFee: [source, mint, dest, authority] amount @2, decimals @10, fee @11
          if (d[1] === 1) {
            ix.label = "Token2022.transferCheckedWithFee";
            const mv: TokenMove = { mint: ix.accounts[1], amount: u64(d, 2), source: ix.accounts[0], dest: ix.accounts[2] };
            if (ix.accounts[3] === owner) out.tokenOut.push(mv); else inflow(mv);
          } else {
            ix.label = `Token2022.transferFee.${d[1]}`;
            out.dangers.push({ kind: "unknown_instruction", program: prog, detail: `transfer-fee instruction ${d[1]} is not understood` });
          }
          break;
        }
        case 1: ix.label = "Token.initializeAccount"; break;
        case 18: ix.label = "Token.initializeAccount3"; break;
        case 17: ix.label = "Token.syncNative"; break;
        default:
          ix.label = `Token.${kind}`;
          // the controller's parser (solmsg.c) treats the same set as "nothing leaves"; everything else is an
          // instruction the display cannot show, so it is declared and needs a conscious touch
          if (!TOKEN_BENIGN.has(kind)) out.dangers.push({ kind: "unknown_instruction", program: prog, detail: `Token instruction ${kind} is not understood` });
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
  out.solRecipients = [...lamportsTo.values()].filter((n) => n > 0n).length;
  out.dest = best?.[0] ?? null;
  // the fee: 5000 per required signature, plus the compute budget's unit limit x price. Without an explicit
  // limit the runtime grants 200k units per non-budget instruction (capped at 1.4M), the same way solmsg.c counts it.
  let cuLimit: bigint | null = null;
  let cuPrice = 0n;
  let budgetIxs = 0;
  for (const ix of instructions) {
    if (ix.program !== COMPUTE_BUDGET) continue;
    budgetIxs++;
    if (ix.data[0] === 2 && ix.data.length >= 5) cuLimit = BigInt(u32(ix.data, 1));
    else if (ix.data[0] === 3 && ix.data.length >= 9) cuPrice = u64(ix.data, 1);
  }
  let limit = cuLimit ?? DEFAULT_CU_PER_IX * BigInt(Math.max(0, instructions.length - budgetIxs));
  if (limit > MAX_CU) limit = MAX_CU;
  out.priorityLamports = limit > 0n ? (limit * cuPrice + 999_999n) / 1_000_000n : 0n;
  // version 1: the priority fee is a flat lamport amount in the header, and ComputeBudget instructions are ignored
  if (msg.v1) out.priorityLamports = msg.v1.priorityLamports;
  out.feeLamports = BASE_FEE_PER_SIG * BigInt(header.numRequiredSignatures) + out.priorityLamports;
  return out;
}

/** The `claims` object the daemon sends to the controller alongside the bytes. */
/** The programs the controller treats as core (solmsg.c): everything else is "unknown_program" unless it is a router. */
export const CONTROLLER_CORE_PROGRAMS = new Set([SYSTEM, TOKEN, TOKEN_2022, ATA, COMPUTE_BUDGET, MEMO]);

/** The owner's associated token accounts for a mint under both token programs (web3.js only, no spl-token). */
export function ownerAtaSet(owner: string, mint: string, cache?: Map<string, string>): Set<string> {
  const out = new Set<string>();
  let o: PublicKey, m: PublicKey;
  try { o = new PublicKey(owner); m = new PublicKey(mint); } catch { return out; }
  for (const prog of [TOKEN, TOKEN_2022]) {
    const key = `${owner}:${mint}:${prog}`;
    let ata = cache?.get(key);
    if (!ata) {
      try { ata = PublicKey.findProgramAddressSync([o.toBytes(), new PublicKey(prog).toBytes(), m.toBytes()], new PublicKey(ATA))[0].toBase58(); } catch { continue; }
      cache?.set(key, ata);
    }
    out.add(ata);
  }
  return out;
}

export function claimsFor(p: ParsedTx, routers: ReadonlySet<string> = new Set([JUPITER_V6])) {
  // the controller knows these names (solmsg.c DANGER_NAMES); its parser sees a nonce handover as set_authority
  // and a burn as a token outflow, so those are translated or dropped rather than sent as words it would refuse
  const dangerous = new Set<string>(p.dangers.map((d) => (d.kind === "nonce" ? "set_authority" : d.kind)).filter((k) => k !== "burn"));
  // a program that is neither core nor an allowlisted router is "unknown_program" on the controller and must be declared
  if (p.programs.some((x) => !CONTROLLER_CORE_PROGRAMS.has(x) && !routers.has(x))) dangerous.add("unknown_program");
  // resolving the tables lets the daemon explain the transaction; the controller still sees those instructions as opaque
  if (p.touchesLookups) dangerous.add("lookup_table");
  return {
    lamports_out: p.lamportsOut.toString(),
    token_out: p.tokenOut.map((t) => ({ mint: t.mint ?? "", amount: t.amount.toString(), dest: t.dest ?? "" })),
    dest: p.dest ?? "",
    programs: p.programs,
    dangerous: [...dangerous],
    fee_lamports: p.feeLamports.toString(),
  };
}

export type { Message, MessageV0 };

/** Maximum token transfers the controller lists on its display; more and it refuses the whole request. */
export const CONTROLLER_MAX_TOKEN_OUT = 8;

/** Shapes the controller refuses outright (PROTOCOL §1), said up front instead of coming back as denied:policy. */
export function controllerLimits(tx: ParsedTx): string[] {
  const out: string[] = [];
  if (tx.solRecipients > 1) out.push(`SOL leaves to ${tx.solRecipients} different recipients; the controller shows one destination and refuses more.`);
  if (tx.tokenOut.length > CONTROLLER_MAX_TOKEN_OUT) out.push(`${tx.tokenOut.length} token transfers; the controller lists at most ${CONTROLLER_MAX_TOKEN_OUT}.`);
  return out;
}
