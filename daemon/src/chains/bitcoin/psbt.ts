/**
 * Bitcoin PSBTs: build (coin selection, fee, change) and parse (owner-centric view of any PSBT a dapp or
 * the swap module hands us). The daemon never signs: the controller returns partial signatures and the
 * daemon finalises. P2WPKH only, mainnet or testnet.
 */
import * as bitcoin from "bitcoinjs-lib";
import type { ChainDanger, ChainParsed, ChainTokenMove } from "../types.js";
import type { Utxo } from "./esplora.js";

export type BtcNetwork = "mainnet" | "testnet";
export const netOf = (n: BtcNetwork) => (n === "mainnet" ? bitcoin.networks.bitcoin : bitcoin.networks.testnet);

/** Below this a change output costs more to spend than it is worth; it goes to the miners instead. */
export const DUST_SATS = 546;
/** Inputs this small are usually someone's tracking dust. */
export const DUST_INPUT_SATS = 1000;
const RBF_SEQUENCE = 0xfffffffd;

export interface BtcOutput { address: string | null; script: Buffer; value: number; isChange: boolean; opReturn: Buffer | null }

export interface BtcParsed extends ChainParsed {
  chain: "bitcoin";
  network: BtcNetwork;
  /** Inputs the owner's key spends, with their values from witness_utxo. */
  ownerInputs: { index: number; value: number; txid: string; vout: number }[];
  /** Inputs that belong to someone else (coinjoin / payjoin); the controller cannot sign them. */
  foreignInputs: number;
  /** Inputs the owner spends whose value the PSBT does not carry; fee cannot be computed → blocked. */
  inputsWithoutValue: number;
  outputs: BtcOutput[];
  satsOut: bigint;
  feeSats: bigint;
  changeSats: bigint;
  rbf: boolean;
  sighashAll: boolean;
  memo: string | null;
  psbtBase64: string;
}

/** vbytes for a P2WPKH spend: overhead + inputs + outputs (+ OP_RETURN). */
export function estimateVsize(nIn: number, outputs: { kind: "p2wpkh" | "p2tr" | "p2wsh" | "p2pkh" | "p2sh" | "opreturn"; len?: number }[]): number {
  let v = 10.5 + nIn * 68;
  for (const o of outputs) v += o.kind === "p2wpkh" ? 31 : o.kind === "p2tr" || o.kind === "p2wsh" ? 43 : o.kind === "p2pkh" ? 34 : o.kind === "p2sh" ? 32 : 9 + 2 + (o.len ?? 0);
  return Math.ceil(v);
}

function outputKind(address: string, network: BtcNetwork): "p2wpkh" | "p2tr" | "p2wsh" | "p2pkh" | "p2sh" {
  const script = bitcoin.address.toOutputScript(address, netOf(network));
  if (script.length === 22 && script[0] === 0x00) return "p2wpkh";
  if (script.length === 34 && script[0] === 0x51) return "p2tr";
  if (script.length === 34 && script[0] === 0x00) return "p2wsh";
  if (script.length === 25) return "p2pkh";
  return "p2sh";
}

export interface BuildRequest {
  network: BtcNetwork;
  owner: string;
  dest: string;
  amountSats: bigint;
  utxos: Utxo[];
  feeRate: number; // sat/vB
  /** OP_RETURN payload (THORChain memo); ≤ 80 bytes. */
  memo?: string;
  /** Spend unconfirmed coins too (default: confirmed only). */
  allowUnconfirmed?: boolean;
  rbf?: boolean;
}

export interface BuildResult { psbt: bitcoin.Psbt; feeSats: bigint; changeSats: bigint; inputs: Utxo[]; vsize: number }

/** Largest-first coin selection, change ≥ dust or folded into the fee. Throws when funds are insufficient. */
export function buildPsbt(req: BuildRequest): BuildResult {
  const network = netOf(req.network);
  if (req.amountSats <= 0n) throw new Error("amount must be positive");
  if (req.memo && Buffer.from(req.memo, "utf8").length > 80) throw new Error("memo longer than 80 bytes");
  const ownerScript = bitcoin.address.toOutputScript(req.owner, network);
  if (!(ownerScript.length === 22 && ownerScript[0] === 0x00)) throw new Error("owner address must be native segwit (P2WPKH)");
  void bitcoin.address.toOutputScript(req.dest, network); // validates the destination for this network
  const candidates = req.utxos.filter((u) => req.allowUnconfirmed || u.confirmed).sort((a, b) => b.value - a.value);
  const destKind = outputKind(req.dest, req.network);
  const memoLen = req.memo ? Buffer.from(req.memo, "utf8").length : 0;
  const outs = (withChange: boolean) => [{ kind: destKind }, ...(withChange ? [{ kind: "p2wpkh" as const }] : []), ...(req.memo ? [{ kind: "opreturn" as const, len: memoLen }] : [])];

  const chosen: Utxo[] = [];
  let sum = 0n;
  let fee = 0n;
  let change = 0n;
  let vsize = 0;
  let ok = false;
  for (const u of candidates) {
    chosen.push(u); sum += BigInt(u.value);
    vsize = estimateVsize(chosen.length, outs(true));
    fee = BigInt(Math.ceil(vsize * req.feeRate));
    if (sum >= req.amountSats + fee) {
      change = sum - req.amountSats - fee;
      if (change < BigInt(DUST_SATS)) { // no change output: the remainder goes to the fee
        vsize = estimateVsize(chosen.length, outs(false));
        fee = sum - req.amountSats;
        change = 0n;
      }
      ok = true; break;
    }
  }
  if (!ok) throw new Error(`insufficient funds: have ${sum} sats across ${candidates.length} spendable coins, need ${req.amountSats} plus fee`);

  const psbt = new bitcoin.Psbt({ network });
  psbt.setVersion(2);
  for (const u of chosen) psbt.addInput({ hash: u.txid, index: u.vout, sequence: req.rbf === false ? 0xffffffff : RBF_SEQUENCE, witnessUtxo: { script: ownerScript, value: u.value } });
  psbt.addOutput({ address: req.dest, value: Number(req.amountSats) });
  if (change > 0n) psbt.addOutput({ address: req.owner, value: Number(change) });
  if (req.memo) psbt.addOutput({ script: bitcoin.payments.embed({ data: [Buffer.from(req.memo, "utf8")] }).output!, value: 0 });
  return { psbt, feeSats: fee, changeSats: change, inputs: chosen, vsize };
}

/** Owner-centric view of a PSBT: what leaves, where, what it costs, and what is odd. */
export function parsePsbt(bytes: Uint8Array, owner: string, network: BtcNetwork): BtcParsed {
  const net = netOf(network);
  const psbt = bitcoin.Psbt.fromBuffer(Buffer.from(bytes), { network: net });
  const ownerScript = bitcoin.address.toOutputScript(owner, net);
  const ownerInputs: BtcParsed["ownerInputs"] = [];
  let foreignInputs = 0, inputsWithoutValue = 0;
  let inSum = 0n;
  let rbf = false;
  let sighashAll = true;
  const dangers: ChainDanger[] = [];
  psbt.txInputs.forEach((tin, index) => {
    const d = psbt.data.inputs[index];
    if ((tin.sequence ?? 0xffffffff) < 0xfffffffe) rbf = true;
    if (d.sighashType != null && d.sighashType !== bitcoin.Transaction.SIGHASH_ALL) sighashAll = false;
    const wu = d.witnessUtxo;
    let script: Buffer | null = wu?.script ?? null;
    let value: number | null = wu?.value ?? null;
    if (!wu && d.nonWitnessUtxo) {
      try { const prev = bitcoin.Transaction.fromBuffer(d.nonWitnessUtxo); const o = prev.outs[tin.index]; script = o.script; value = o.value; } catch { /* unreadable previous tx */ }
    }
    if (script && script.equals(ownerScript)) {
      if (value == null) inputsWithoutValue++;
      else { inSum += BigInt(value); ownerInputs.push({ index, value, txid: Buffer.from(tin.hash).reverse().toString("hex"), vout: tin.index }); }
    } else if (script) foreignInputs++;
    else { inputsWithoutValue++; } // unknown script and no value: cannot even tell whose it is
  });
  if (!sighashAll) dangers.push({ kind: "sighash_not_all", detail: "an input uses a sighash flag other than ALL, so outputs could be changed after signing" });

  const outputs: BtcOutput[] = psbt.txOutputs.map((o) => {
    const opReturn = o.script.length >= 2 && o.script[0] === 0x6a ? (bitcoin.script.decompile(o.script)?.find((c) => Buffer.isBuffer(c)) as Buffer | undefined) ?? Buffer.alloc(0) : null;
    let address: string | null = null;
    try { address = opReturn ? null : bitcoin.address.fromOutputScript(o.script, net); } catch { address = null; }
    return { address, script: o.script, value: o.value, isChange: o.script.equals(ownerScript), opReturn };
  });
  let memo: string | null = null;
  for (const o of outputs) if (o.opReturn) { memo = o.opReturn.toString("utf8").slice(0, 200); if (o.value > 0) dangers.push({ kind: "unknown_change", detail: "burns coins into an OP_RETURN output" }); }
  const paying = outputs.filter((o) => !o.isChange && !o.opReturn);
  const outSum = outputs.reduce((a, o) => a + BigInt(o.value), 0n);
  const changeSats = outputs.filter((o) => o.isChange).reduce((a, o) => a + BigInt(o.value), 0n);
  const satsOut = paying.reduce((a, o) => a + BigInt(o.value), 0n);
  const best = [...paying].sort((a, b) => b.value - a.value)[0];
  const dest = best?.address ?? null;
  if (paying.length > 1) dangers.push({ kind: "unknown_change", detail: `pays ${paying.length} different addresses; I can only account for one destination and your change` });
  if (paying.some((o) => !o.address)) dangers.push({ kind: "unknown_change", detail: "pays to a script I cannot turn into an address" });
  if (inputsWithoutValue > 0) dangers.push({ kind: "unknown_change", detail: `${inputsWithoutValue} input${inputsWithoutValue > 1 ? "s" : ""} carry no value, so I cannot compute the fee` });
  const feeSats = inputsWithoutValue > 0 || foreignInputs > 0 ? -1n : inSum - outSum;
  if (feeSats < 0n && inputsWithoutValue === 0 && foreignInputs === 0) dangers.push({ kind: "unknown_change", detail: "outputs exceed inputs; the PSBT is malformed" });

  const tokenOut: ChainTokenMove[] = [];
  return {
    chain: "bitcoin", network, ownerInputs, foreignInputs, inputsWithoutValue, outputs, satsOut, feeSats, changeSats, rbf, sighashAll, memo, psbtBase64: psbt.toBase64(),
    nativeOut: satsOut, nativeUnit: "sats", dest, tokenOut, dangers, programs: [], signers: ownerInputs.length ? [owner] : [],
    counterparties: paying.map((o) => o.address).filter((a): a is string => !!a && a !== owner),
  };
}

/** After the controller returned the PSBT with its partial signatures: verify, finalise, extract. */
export function finalisePsbt(signedPsbt: Uint8Array, network: BtcNetwork, verify: (pubkey: Buffer, msghash: Buffer, signature: Buffer) => boolean): { txHex: string; txid: string; vsize: number } {
  const psbt = bitcoin.Psbt.fromBuffer(Buffer.from(signedPsbt), { network: netOf(network) });
  for (let i = 0; i < psbt.data.inputs.length; i++) {
    if (!psbt.data.inputs[i].partialSig?.length) throw new Error(`input ${i} has no signature`);
    if (!psbt.validateSignaturesOfInput(i, verify)) throw new Error(`input ${i} has an invalid signature`);
  }
  psbt.finalizeAllInputs();
  const tx = psbt.extractTransaction();
  return { txHex: tx.toHex(), txid: tx.getId(), vsize: tx.virtualSize() };
}

/** The unsigned PSBT and the controller's answer must describe the same transaction (same inputs and outputs). */
export function sameTransaction(a: Uint8Array, b: Uint8Array, network: BtcNetwork): boolean {
  const net = netOf(network);
  const pa = bitcoin.Psbt.fromBuffer(Buffer.from(a), { network: net });
  const pb = bitcoin.Psbt.fromBuffer(Buffer.from(b), { network: net });
  const ta = (pa as any).__CACHE.__TX as bitcoin.Transaction;
  const tb = (pb as any).__CACHE.__TX as bitcoin.Transaction;
  return ta.toBuffer().equals(tb.toBuffer());
}
