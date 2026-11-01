// builds the vector files from public chain data. run: node vectors/build.mjs [name...]
// the expectations are decoded here from the node's / explorer's own view of each transaction (the parsed
// instruction list a Solana node returns, the prevouts an explorer returns, the rlp fields of a receipt),
// never from this repository's parsers. the json this writes is what the daemon and firmware tests assert.
import { createRequire } from "node:module";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const require = createRequire(join(HERE, "..", "daemon", "package.json"));
const viem = require("viem");
const bitcoin = require("bitcoinjs-lib");
const bs58 = require("bs58").default ?? require("bs58");

const SOL_RPC = "https://api.mainnet-beta.solana.com";
const ETH_RPC = "https://ethereum-rpc.publicnode.com";
const MEMPOOL = "https://mempool.space/api";
const SOLSCAN = (sig) => `https://solscan.io/tx/${sig}`;
const ETHERSCAN = (h) => `https://etherscan.io/tx/${h}`;
const MEMPOOL_TX = (t) => `https://mempool.space/tx/${t}`;

const sleep = (ms) => new Promise((f) => setTimeout(f, ms));
const jsonRpc = async (url, method, params) => {
  for (let attempt = 0; ; attempt++) {
    const r = await (await fetch(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method, params }) })).json();
    if (r.error?.code === 429 && attempt < 8) { await sleep(1500 * (attempt + 1)); continue; } // the public rpc rate limits
    if (r.error) throw new Error(`${method}: ${JSON.stringify(r.error)}`);
    if (url === SOL_RPC) await sleep(400);
    return r.result;
  }
};
const b64 = (u8) => Buffer.from(u8).toString("base64");
const only = process.argv.slice(2);
const write = (chain, name, vector) => {
  if (only.length && !only.includes(name)) return;
  const dir = join(HERE, chain);
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `${name}.json`), JSON.stringify(vector, (_, v) => (typeof v === "bigint" ? v.toString() : v), 2) + "\n");
  console.log(`${chain}/${name}`);
};

// ---------------------------------------------------------------- solana
const SOL_CORE = new Set([
  "11111111111111111111111111111111", "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb",
  "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL", "ComputeBudget111111111111111111111111111111", "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr",
]);
const JUPITER = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4";

/** the message bytes of a base64 transaction: drop the shortvec-counted signatures in front */
function messageOf(txB64) {
  const raw = Buffer.from(txB64, "base64");
  let n = 0, shift = 0, i = 0;
  for (;;) { const b = raw[i++]; n |= (b & 0x7f) << shift; if (!(b & 0x80)) break; shift += 7; }
  return raw.subarray(i + 64 * n);
}

async function lookupTablesOf(message) {
  const out = {};
  for (const l of message.addressTableLookups ?? []) {
    const acc = await jsonRpc(SOL_RPC, "getAccountInfo", [l.accountKey, { encoding: "base64" }]);
    const data = Buffer.from(acc.value.data[0], "base64");
    const addrs = [];
    for (let o = 56; o + 32 <= data.length; o += 32) addrs.push(bs58.encode(data.subarray(o, o + 32)));
    out[l.accountKey] = addrs;
  }
  return out;
}

async function solana(name, signature, notes) {
  const raw = await jsonRpc(SOL_RPC, "getTransaction", [signature, { encoding: "base64", maxSupportedTransactionVersion: 0 }]);
  const parsed = await jsonRpc(SOL_RPC, "getTransaction", [signature, { encoding: "jsonParsed", maxSupportedTransactionVersion: 0 }]);
  const msg = parsed.transaction.message;
  const owner = msg.accountKeys[0].pubkey;
  const bytes = messageOf(raw.transaction[0]);
  const programs = [...new Set(msg.instructions.map((ix) => ix.programId))];
  let lamportsOut = 0n;
  const lamportsTo = new Map();
  const tokenOut = [];
  const warnings = new Set();
  let durableNonce = false;
  msg.instructions.forEach((ix, idx) => {
    const p = ix.parsed;
    if (ix.program === "system" && p) {
      const t = p.type, i = p.info;
      if (t === "advanceNonce" && idx === 0) durableNonce = true;
      if ((t === "transfer" || t === "createAccount" || t === "createAccountWithSeed") && i.source === owner) {
        const n = BigInt(i.lamports); lamportsOut += n; const to = i.newAccount ?? i.destination; lamportsTo.set(to, (lamportsTo.get(to) ?? 0n) + n);
      }
      if (t === "transferWithSeed" && i.sourceBase === owner) { const n = BigInt(i.lamports); lamportsOut += n; lamportsTo.set(i.destination, (lamportsTo.get(i.destination) ?? 0n) + n); }
      if (t === "authorizeNonce" && i.nonceAuthority === owner) warnings.add("set_authority");
      if (t === "assign" && i.account === owner) warnings.add("assign");
    }
    if ((ix.program === "spl-token" || ix.program === "spl-token-2022") && p) {
      const t = p.type, i = p.info;
      const authority = i.authority ?? i.multisigAuthority;
      if ((t === "transfer" || t === "transferChecked") && authority === owner) tokenOut.push({ mint: i.mint ?? null, mint_in_instruction: t === "transferChecked", dest: i.destination, amount: i.amount ?? i.tokenAmount.amount });
      if ((t === "approve" || t === "approveChecked") && i.owner === owner) warnings.add("approve");
      if (t === "setAuthority" && i.authority === owner) warnings.add("set_authority");
      if (t === "closeAccount" && (i.owner === owner || i.account === owner)) warnings.add("close_account"); // protocol §1: any close of an owner-held account is declared, wherever the rent goes
    }
  });
  if (programs.some((x) => !SOL_CORE.has(x) && x !== JUPITER)) warnings.add("unknown_program");
  let dest = null, best = -1n;
  for (const [k, v] of lamportsTo) if (v > best) { best = v; dest = k; }
  if (!dest && tokenOut.length) dest = tokenOut[0].dest;
  const lookups = msg.addressTableLookups?.length ? await lookupTablesOf(msg) : undefined;
  write("solana", name, {
    chain: "solana", kind: "tx", name, notes,
    source: { signature, slot: parsed.slot, url: SOLSCAN(signature), fetched: "getTransaction base64 + jsonParsed, api.mainnet-beta.solana.com" },
    bytes: b64(bytes), owner, ...(lookups ? { lookup_tables: lookups } : {}),
    expect: {
      version: msg.addressTableLookups ? 0 : "legacy", signers: msg.accountKeys.filter((k) => k.signer).length,
      lamports_out: lamportsOut.toString(), dest, token_out: tokenOut, programs, fee_lamports: String(parsed.meta.fee),
      durable_nonce: durableNonce, warnings: [...warnings],
      lamport_recipients: lamportsTo.size,
      controller: lamportsTo.size > 1 ? "refuse: multi_dest (sol leaves towards more than one recipient; the display shows one)" : tokenOut.length > 8 ? "refuse: token_out_overflow (the controller lists at most 8 token transfers)" : lookups ? "accept_with_declaration: lookup_table (the controller cannot resolve lookup tables; every instruction that touches one is opaque to it)" : warnings.size ? `accept_with_declaration: ${[...warnings].join(", ")}` : "accept",
    },
  });
}

// ---------------------------------------------------------------- evm
const MAX_U256 = (1n << 256n) - 1n;
const ROUTERS_CONTROLLER = new Set(["0x111111125421ca6dc452d289314280a0f8842a65", "0xdef1c0ded9bec7f1a1670819833240f027b25eff"]);
const addr = (word) => viem.getAddress("0x" + word.slice(-40));

async function evm(name, hash, notes) {
  const t = await jsonRpc(ETH_RPC, "eth_getTransactionByHash", [hash]);
  if (!t) throw new Error(`no tx ${hash}`);
  const type = parseInt(t.type, 16);
  const chainId = Number(t.chainId);
  const base = { chainId, nonce: Number(t.nonce), gas: BigInt(t.gas), to: t.to ? viem.getAddress(t.to) : undefined, value: BigInt(t.value), data: t.input };
  let tx, sig;
  if (type === 0) { tx = { ...base, type: "legacy", gasPrice: BigInt(t.gasPrice) }; sig = { r: t.r, s: t.s, v: BigInt(t.v) }; }
  else if (type === 2) { tx = { ...base, type: "eip1559", maxFeePerGas: BigInt(t.maxFeePerGas), maxPriorityFeePerGas: BigInt(t.maxPriorityFeePerGas), accessList: t.accessList ?? [] }; sig = { r: t.r, s: t.s, yParity: Number(t.yParity ?? t.v) }; }
  else throw new Error(`type ${type} not wanted`);
  // the signed serialisation must hash to the transaction hash, or the fields are not what the chain saw
  if (viem.keccak256(viem.serializeTransaction(tx, sig)) !== hash) throw new Error(`hash mismatch for ${hash}`);
  const unsigned = viem.hexToBytes(viem.serializeTransaction(tx));
  const owner = viem.getAddress(t.from);
  const data = t.input, sel = data.slice(0, 10), args = data.slice(10);
  const word = (i) => args.slice(64 * i, 64 * (i + 1));
  const warnings = new Set();
  let call = "none", tokenOut = [], spender = null, allowance = null, callTo = null;
  if (!t.to) { call = "create"; warnings.add("unknown_contract"); }
  else if (data === "0x") call = "none";
  else if (sel === "0xa9059cbb" && args.length === 128) { call = "transfer"; callTo = addr(word(0)); tokenOut = [{ token: base.to, to: callTo, amount: BigInt("0x" + word(1)).toString() }]; }
  else if (sel === "0x095ea7b3" && args.length === 128) { call = "approve"; spender = addr(word(0)); allowance = BigInt("0x" + word(1)); warnings.add(allowance === MAX_U256 ? "approve_unlimited" : "approve"); }
  else if (sel === "0x23b872dd" && args.length === 192) { call = "transferFrom"; const from = addr(word(0)); callTo = addr(word(1)); if (from === owner) tokenOut = [{ token: base.to, to: callTo, amount: BigInt("0x" + word(2)).toString() }]; }
  else if (sel === "0xa22cb465" && args.length === 128) { call = "setApprovalForAll"; callTo = addr(word(0)); if (BigInt("0x" + word(1)) === 1n) warnings.add("set_approval_for_all"); }
  else if (sel === "0x39509351" && args.length === 128) { call = "increaseAllowance"; spender = addr(word(0)); allowance = BigInt("0x" + word(1)); warnings.add("approve"); }
  else { call = ROUTERS_CONTROLLER.has(t.to.toLowerCase()) ? "router" : "unknown"; if (call === "unknown") warnings.add("unknown_contract"); }
  const maxFeePerGas = type === 0 ? BigInt(t.gasPrice) : BigInt(t.maxFeePerGas);
  write("evm", name, {
    chain: "evm", kind: "evm_tx", name, notes,
    source: { hash, block: Number(t.blockNumber), url: ETHERSCAN(hash), fetched: "eth_getTransactionByHash, ethereum-rpc.publicnode.com; unsigned payload re-serialised and the signed form checked against the hash" },
    bytes: b64(unsigned), owner,
    expect: {
      type: type === 0 ? "legacy" : "eip1559", chain_id: chainId, nonce: Number(t.nonce), to: base.to ?? null, value_wei: base.value.toString(),
      gas_limit: base.gas.toString(), max_fee_per_gas_wei: maxFeePerGas.toString(), max_fee_wei: (base.gas * maxFeePerGas).toString(),
      call, call_to: callTo, token_out: tokenOut, spender, allowance: allowance == null ? null : allowance.toString(), warnings: [...warnings],
      controller: warnings.size ? `accept_with_declaration: ${[...warnings].join(", ")}` : "accept",
    },
  });
}

// ---------------------------------------------------------------- bitcoin
async function btc(name, txid, notes) {
  const t = await (await fetch(`${MEMPOOL}/tx/${txid}`)).json();
  const inAddrs = new Set(t.vin.map((v) => v.prevout.scriptpubkey_address));
  if (inAddrs.size !== 1) throw new Error(`${txid}: inputs from more than one address`);
  const ownerAddress = [...inAddrs][0];
  const ownerPub = Buffer.from(t.vin[0].witness[1], "hex");
  const ownerScript = Buffer.from(t.vin[0].prevout.scriptpubkey, "hex");
  if (!(ownerScript.length === 22 && ownerScript[0] === 0 && ownerScript[1] === 0x14)) throw new Error("not p2wpkh");
  if (!bitcoin.crypto.hash160(ownerPub).equals(ownerScript.subarray(2))) throw new Error("witness key does not pay the input script");
  const psbt = new bitcoin.Psbt({ network: bitcoin.networks.bitcoin });
  psbt.setVersion(t.version); psbt.setLocktime(t.locktime);
  const deriv = { masterFingerprint: Buffer.from("00000000", "hex"), pubkey: ownerPub, path: "m/84'/0'/0'/0/0" };
  for (const v of t.vin) psbt.addInput({ hash: v.txid, index: v.vout, sequence: v.sequence, witnessUtxo: { script: Buffer.from(v.prevout.scriptpubkey, "hex"), value: v.prevout.value }, bip32Derivation: [deriv] });
  for (const o of t.vout) psbt.addOutput({ script: Buffer.from(o.scriptpubkey, "hex"), value: o.value, ...(o.scriptpubkey_address === ownerAddress ? { bip32Derivation: [deriv] } : {}) });
  const paying = t.vout.filter((o) => o.scriptpubkey_address !== ownerAddress);
  const satsOut = paying.reduce((a, o) => a + o.value, 0);
  const change = t.vout.filter((o) => o.scriptpubkey_address === ownerAddress).reduce((a, o) => a + o.value, 0);
  const inSum = t.vin.reduce((a, v) => a + v.prevout.value, 0);
  const fee = inSum - t.vout.reduce((a, o) => a + o.value, 0);
  if (fee !== t.fee) throw new Error("fee disagrees with the explorer");
  const warnings = new Set();
  if (satsOut === 0 ? fee > 0 : fee * 10 >= satsOut) warnings.add("high_fee");
  if (paying.length > 1) warnings.add("unknown_change");
  const best = [...paying].sort((a, b) => b.value - a.value)[0];
  write("bitcoin", name, {
    chain: "bitcoin", kind: "btc_psbt", name, notes,
    source: { txid, url: MEMPOOL_TX(txid), fetched: "mempool.space /api/tx (prevouts, witnesses); psbt v0 built from it with the spender's key named on every input" },
    bytes: psbt.toBase64(), owner: { address: ownerAddress, pubkey: ownerPub.toString("hex") },
    expect: {
      inputs: t.vin.length, owner_inputs: t.vin.length, outputs: t.vout.length, sats_in: String(inSum), sats_out: String(satsOut), fee_sats: String(fee), change_sats: String(change),
      dest: best?.scriptpubkey_address ?? null, dest_script: best?.scriptpubkey ?? null, warnings: [...warnings],
      controller: warnings.size ? `accept_with_declaration: ${[...warnings].join(", ")}` : "accept",
    },
  });
}

function psbtV2() {
  // bip-370: magic, global map {PSBT_GLOBAL_TX_VERSION 0x02: 2, PSBT_GLOBAL_INPUT_COUNT 0x04: 0, PSBT_GLOBAL_OUTPUT_COUNT 0x05: 0, PSBT_GLOBAL_VERSION 0xfb: 2}, no unsigned tx
  const hex = "70736274ff" + "0102" + "0402000000" + "0104" + "0100" + "0105" + "0100" + "01fb" + "0402000000" + "00";
  write("bitcoin", "psbt_v2_refused", {
    chain: "bitcoin", kind: "btc_psbt", name: "psbt_v2_refused", notes: "a psbt version 2 (bip-370) with no inputs or outputs; the controller only reads version 0",
    source: { url: "https://github.com/bitcoin/bips/blob/master/bip-0370.mediawiki", fetched: "constructed from the bip" },
    bytes: Buffer.from(hex, "hex").toString("base64"), owner: { address: null, pubkey: null },
    expect: { warnings: [], controller: "refuse: psbt_version" },
  });
}

// ---------------------------------------------------------------- eip-712
const DOMAIN_TYPE = [{ name: "name", type: "string" }, { name: "version", type: "string" }, { name: "chainId", type: "uint256" }, { name: "verifyingContract", type: "address" }];
function typed(name, typedData, notes, expectExtra, sourceUrl) {
  typedData = { ...typedData, types: { EIP712Domain: DOMAIN_TYPE, ...typedData.types } }; // as eth_signTypedData_v4 carries it
  const digest = viem.hashTypedData(typedData);
  write("evm_typed", name, {
    chain: "evm", kind: "evm_typed_data", name, notes, source: { url: sourceUrl }, typed_data: typedData, owner: expectExtra.owner ?? null,
    expect: { digest, ...expectExtra },
  });
}

const USDC = "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48";
const PERMIT_TYPES = { Permit: [{ name: "owner", type: "address" }, { name: "spender", type: "address" }, { name: "value", type: "uint256" }, { name: "nonce", type: "uint256" }, { name: "deadline", type: "uint256" }] };

async function main() {
  // solana: recent mainnet transactions picked from a block scan for their shape (signature in each file)
  await solana("sol_transfer", "2NsGbn3Fv1GUSUNgQfm7d7FwvboTakoop3bFrFbBXr9of24byhzoKJeZpsFHjmgi9vuqw4bWmvS5siGkv2gyvkzd", "a plain system transfer, one signer");
  await solana("sol_transfer_memo_v0", "4FWBNLR2Jpb97drFFtryyfLjmNaqgNJhosHSczLCx7yTiYUtNzuyeQPLKZHtesFPWm7DXpGKiBH8WiULUzd9amkn", "a system transfer with a memo in a v0 message that uses no lookup tables");
  await solana("spl_transfer_checked_2022", "3SnRxEWpk65A9dU6rvcxyW4cdKvVqVaVeVrUWd4c892t2nDwurcoYrWTdLhbvZPAHCq9FJ4H1mJnmwpnfXKgG41G", "token-2022 transferChecked with a compute budget");
  await solana("spl_transfer_plain", "WFnfeKFhLtw1sWLE3DSoJo6TVmALGrZHjYBstCvatySZHBRTLKjnQ6hNjVe2Sub8mBH3jTdwYutRZocQvg6Hidj", "twenty-one token transfers (tag 3, no mint in the instruction) in one message: more than the controller lists");
  await solana("jupiter_swap_v0", "5vKHp2j2kCEX7d86Z7DgMsiDeQknLXrbdZovkToViV8AMymiaeC6SzQNYonFC3C6kjjeTYfzwfkCxphUggyD88ge", "a jupiter v6 route in a v0 message with three lookup tables");
  await solana("nonce_advance_v0", "5vFiKVZS3QtqG1r7nRyQAuEeA3v8bjeietNd1VHSAA4maMyZENSicmmmix5dQpDwTnRAZi82nTxeUMuXNvnkUofq", "a durable-nonce transaction: advanceNonce first, then a program the controller does not know, through lookup tables");
  await solana("unknown_program", "4XAW25cL9yUBPTgRFYXyWyucv4hP9Ahb5yaHXLGmvQm9r2Y5Nmaymrp9qfjffubUQKNRwj4PRPJX7XwZ2HMMfhhf", "one instruction to a program that is neither core nor a router");
  await solana("pump_buy", "465Qhqe7B2X2LZFYqSKMd37AwPwEvW1FYcvfAg3MQCyPwkWtwvbcQQ3LLdZBjLaJSg4fP9sC4z1ouo8r5xWYm1dm", "a pump.fun trade: an unknown program next to token-2022 and system instructions");
  // evm: recent mainnet transactions picked from a block scan
  await evm("eth_transfer_legacy", "0xd31dff1e7a09d5fde9f3c051981a2931ebf604d7157bb3d95437fb3362e020b7", "legacy eip-155 ether transfer, 21000 gas");
  await evm("eth_transfer_1559", "0x584c20ee931146b22d9ff12c80a04ffd450b35968614bd368173c08762792407", "type-2 ether transfer to a contract with no calldata");
  await evm("usdt_transfer", "0xc2991679d563ffc937ec9376b35b6772658fa8765fd72f716792c29c522dc1e6", "erc-20 transfer of usdt");
  await evm("usdc_transfer", "0x0f35ed06b0185c9204c2fa56c74f7b93c929040605fed63dcdfddde82d230531", "erc-20 transfer of usdc");
  await evm("usdt_approve_unlimited", "0x2a660c751cba6924f121f44f9ee27157e28cc1a15107371c06e9d969e94ad3e3", "approve of 2^256-1");
  await evm("usdt_approve_unlimited_legacy", "0x778afcad801a2f32f05648b12a7ba122c2261ea2663aee41b0d25b3d330fba1b", "the same, in a legacy transaction");
  await evm("usdt_approve_finite", "0x52ed08493d0be4b98553bbcf84eb2ef848619905efc6c1e7966e72ba0774a9d7", "approve of a bounded amount");
  await evm("usdc_transfer_from", "0x7bb9f024acad7499e80cb012ab061606664499c97c9f92d000163890d02528a6", "transferFrom pulling someone else's tokens: nothing leaves the signer");
  await evm("uniswap_v2_swap", "0x49279495f6a344974f7223bd2c0f07d5d8720474cc5afd7ee7571182a97f33c9", "a uniswap v2 router call; the router is not in the controller's allowlist");
  await evm("uniswap_swaprouter02", "0x4a94441fc6a6a22eee3b8087ede8a0c3d6e34d02bb249a92f5cdbeb1fa197a7b", "swaprouter02 call carrying ether value");
  await evm("contract_creation", "0xf081b0f29edb9e4cd14939731d6e14214618456c2ae8b1b48877a359cef02616", "no `to`: a contract creation");
  await evm("set_approval_for_all", "0xcbef9749808e18823f1d9202b59569648d16602fca4694550ea4199ea7faa60d", "setApprovalForAll on a collection");
  // bitcoin: recent mainnet p2wpkh spends from one key, rebuilt as unsigned psbts
  await btc("p2wpkh_single_change", "b811cab1382630c454e70adfb52889151f729ab30ef5fac91d7f45cf8f5ecc50", "one input, one payment, change back to the same key");
  await btc("p2wpkh_multi_input_change", "352c4a3df6e373d325916b292a2771ccddf6aa89e1591827de8f259c1eea9b14", "two inputs from the same key, payment to a legacy address, change");
  await btc("p2wpkh_large_payment", "5189f09ea45eff161772e8a415f529494d58f593d6c7fcf69a406b43cc4ccab7", "two inputs, 26.9 btc out");
  await btc("p2wpkh_high_fee", "de191c675c46c93b4f9048d358c9d34cd32668d46037ae91c2205b62bbee4c08", "fee above a tenth of what leaves");
  await btc("p2wpkh_no_change", "1255531cc8c3285e74c75029fc58648343ce0f55e9ae11f6ebe9e09c3e0c6d16", "one input, one output, no change");
  await btc("p2wpkh_no_change_legacy_dest", "f2ef9b72f544429bede9236008a8580554e31e62b6e4b655579798f6d2d82edb", "one input paying a p2pkh address, no change");
  psbtV2();
  // eip-712
  typed("permit_usdc", {
    domain: { name: "USD Coin", version: "2", chainId: 1, verifyingContract: USDC },
    types: PERMIT_TYPES, primaryType: "Permit",
    message: { owner: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", spender: "0x111111125421cA6dc452d289314280a0f8842A65", value: "25000000", nonce: "3", deadline: "1767225600" },
  }, "eip-2612 permit on usdc's real domain: a bounded allowance to the 1inch router", { owner: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", warnings: ["approve"], spender: "0x111111125421cA6dc452d289314280a0f8842A65", allowance: "25000000", controller: "accept_with_declaration: approve" }, "https://eips.ethereum.org/EIPS/eip-2612");
  typed("permit_usdc_unlimited", {
    domain: { name: "USD Coin", version: "2", chainId: 1, verifyingContract: USDC },
    types: PERMIT_TYPES, primaryType: "Permit",
    message: { owner: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", spender: "0x1234567890abcdef1234567890abcdef12345678", value: MAX_U256.toString(), nonce: "4", deadline: "1767225600" },
  }, "eip-2612 permit granting everything, forever, to an unknown spender", { owner: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", warnings: ["approve_unlimited"], spender: "0x1234567890abcdef1234567890abcdef12345678", allowance: MAX_U256.toString(), controller: "accept_with_declaration: approve_unlimited" }, "https://eips.ethereum.org/EIPS/eip-2612");
  typed("seaport_listing", {
    domain: { name: "Seaport", version: "1.6", chainId: 1, verifyingContract: "0x0000000000000068F116a894984e2DB1123eB395" },
    types: {
      OrderComponents: [{ name: "offerer", type: "address" }, { name: "zone", type: "address" }, { name: "offer", type: "OfferItem[]" }, { name: "consideration", type: "ConsiderationItem[]" }, { name: "orderType", type: "uint8" }, { name: "startTime", type: "uint256" }, { name: "endTime", type: "uint256" }, { name: "zoneHash", type: "bytes32" }, { name: "salt", type: "uint256" }, { name: "conduitKey", type: "bytes32" }, { name: "counter", type: "uint256" }],
      OfferItem: [{ name: "itemType", type: "uint8" }, { name: "token", type: "address" }, { name: "identifierOrCriteria", type: "uint256" }, { name: "startAmount", type: "uint256" }, { name: "endAmount", type: "uint256" }],
      ConsiderationItem: [{ name: "itemType", type: "uint8" }, { name: "token", type: "address" }, { name: "identifierOrCriteria", type: "uint256" }, { name: "startAmount", type: "uint256" }, { name: "endAmount", type: "uint256" }, { name: "recipient", type: "address" }],
    },
    primaryType: "OrderComponents",
    message: {
      offerer: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", zone: "0x0000000000000000000000000000000000000000",
      offer: [{ itemType: 2, token: "0xBC4CA0EdA7647A8aB7C2061c2E118A18a936f13D", identifierOrCriteria: "1234", startAmount: "1", endAmount: "1" }],
      consideration: [{ itemType: 0, token: "0x0000000000000000000000000000000000000000", identifierOrCriteria: "0", startAmount: "15000000000000000000", endAmount: "15000000000000000000", recipient: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d" }],
      orderType: 0, startTime: "1767225600", endTime: "1769904000", zoneHash: "0x0000000000000000000000000000000000000000000000000000000000000000", salt: "24446860302761739304752683030156737591518664810215442929", conduitKey: "0x0000007b02230091a7ed01230072f7006a004d60a8d4e71d599b8104250f0000", counter: "0",
    },
  }, "a seaport 1.6 listing of one nft for 15 eth (order shape from the seaport reference); arrays of structs are beyond the controller's typed-data support", { owner: "0x5a2c1e8f2a5b3c0d1e9f8a7b6c5d4e3f2a1b0c9d", warnings: [], market: "Seaport", controller: "refuse: unsupported_typed_data" }, "https://github.com/ProjectOpenSea/seaport/blob/main/docs/SeaportDocumentation.md");
  typed("ether_mail", {
    domain: { name: "Ether Mail", version: "1", chainId: 1, verifyingContract: "0xCcCCccccCCCCcCCCCCCcCcCccCcCCCcCcccccccC" },
    types: { Person: [{ name: "name", type: "string" }, { name: "wallet", type: "address" }], Mail: [{ name: "from", type: "Person" }, { name: "to", type: "Person" }, { name: "contents", type: "string" }] },
    primaryType: "Mail",
    message: { from: { name: "Cow", wallet: "0xCD2a3d9F938E13CD947Ec05AbC7FE734Df8DD826" }, to: { name: "Bob", wallet: "0xbBbBBBBbbBBBbbbBbbBbbbbBBbBbbbbBbBbbBBbB" }, contents: "Hello, Bob!" },
  }, "the eip-712 example from the eip: nested structs, which the controller refuses", { owner: "0xCD2a3d9F938E13CD947Ec05AbC7FE734Df8DD826", warnings: [], controller: "refuse: unsupported_typed_data" }, "https://eips.ethereum.org/EIPS/eip-712");
}

main().catch((e) => { console.error(e); process.exit(1); });
