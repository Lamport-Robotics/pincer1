/**
 * Turns a dapp's `eth_sendTransaction` object into the unsigned RLP the robot signs.
 *
 * The dapp only has to supply `from`/`to`/`value`/`data`; nonce, gas and fees are filled from
 * the RPC the user configured in the popup (never one the dapp picked), so a page cannot feed
 * the signer a stale nonce or absurd fee through its own node. Pure: the RPC call is injected,
 * which is also how the tests run it. Uses viem for RLP and hex; no key material anywhere.
 */
import {
  hexToBigInt,
  hexToNumber,
  isAddress,
  isHex,
  parseTransaction,
  serializeTransaction,
  type AccessList,
  type Address,
  type Hex,
  type TransactionSerializableEIP1559,
  type TransactionSerializableLegacy,
} from 'viem';
import type { EvmTxRequest } from '../protocol';

export type RpcCall = (method: string, params: unknown[]) => Promise<unknown>;

export class EvmTxError extends Error {
  constructor(
    readonly code: 'BAD_REQUEST' | 'NOT_CONNECTED' | 'INTERNAL',
    message: string,
  ) {
    super(message);
    this.name = 'EvmTxError';
  }
}

export interface PreparedEvmTx {
  /** 0x-hex RLP of the unsigned transaction (type-2 `0x02…` or EIP-155 legacy). */
  unsigned: Hex;
  type: 'eip1559' | 'legacy';
  chainId: number;
  nonce: number;
  gas: bigint;
  to: Address | null;
  value: bigint;
  data: Hex;
  maxFeePerGas?: bigint;
  maxPriorityFeePerGas?: bigint;
  gasPrice?: bigint;
}

const GWEI = 1_000_000_000n;

function hexQty(v: string | undefined, what: string): bigint | undefined {
  if (v === undefined || v === null) return undefined;
  if (typeof v !== 'string' || !/^0x[0-9a-fA-F]+$/.test(v)) throw new EvmTxError('BAD_REQUEST', `${what} must be a 0x hex quantity`);
  return hexToBigInt(v as Hex);
}

function asHexData(v: string | undefined, what: string): Hex {
  if (v === undefined || v === null || v === '') return '0x';
  if (typeof v !== 'string' || !isHex(v, { strict: true })) throw new EvmTxError('BAD_REQUEST', `${what} must be 0x hex bytes`);
  if (v.length % 2 !== 0) throw new EvmTxError('BAD_REQUEST', `${what} has an odd number of hex digits`);
  return v;
}

function asAddress(v: unknown, what: string): Address {
  if (typeof v !== 'string' || !isAddress(v, { strict: false })) throw new EvmTxError('BAD_REQUEST', `${what} is not an address`);
  return v as Address;
}

function sameAddress(a: string, b: string): boolean {
  return a.toLowerCase() === b.toLowerCase();
}

function asBigInt(v: unknown, what: string): bigint {
  if (typeof v !== 'string' || !/^0x[0-9a-fA-F]+$/.test(v)) throw new EvmTxError('INTERNAL', `RPC returned a bad ${what}`);
  return hexToBigInt(v as Hex);
}

/**
 * Validate the dapp request, fill what is missing from the RPC, and serialize.
 * `account` is the robot's EVM address: `from` must be it (the robot has only one EVM key).
 */
export async function prepareEvmTransaction(
  req: EvmTxRequest,
  chainId: number,
  account: string,
  rpc: RpcCall,
): Promise<PreparedEvmTx> {
  if (!req || typeof req !== 'object') throw new EvmTxError('BAD_REQUEST', 'transaction must be an object');
  const from = asAddress(req.from, 'from');
  if (!sameAddress(from, account)) throw new EvmTxError('NOT_CONNECTED', `from ${from} is not the Pincer1 account`);
  if (req.chainId !== undefined) {
    const want = /^0x/.test(req.chainId) ? hexToNumber(req.chainId as Hex) : Number(req.chainId);
    if (want !== chainId) throw new EvmTxError('BAD_REQUEST', `transaction is for chain ${want}, wallet is on ${chainId}`);
  }
  const to = req.to === undefined || req.to === null || req.to === '' ? null : asAddress(req.to, 'to');
  const value = hexQty(req.value, 'value') ?? 0n;
  const data = asHexData(req.data ?? req.input, 'data');
  const accessList = req.accessList as AccessList | undefined;

  const nonce =
    hexQty(req.nonce, 'nonce') ??
    asBigInt(await rpc('eth_getTransactionCount', [from, 'pending']), 'nonce');
  if (nonce > BigInt(Number.MAX_SAFE_INTEGER)) throw new EvmTxError('BAD_REQUEST', 'nonce out of range');

  let gas = hexQty(req.gas ?? req.gasLimit, 'gas');
  if (gas === undefined) {
    const est = asBigInt(
      await rpc('eth_estimateGas', [{ from, to: to ?? undefined, value: `0x${value.toString(16)}`, data }]),
      'gas estimate',
    );
    gas = (est * 12n) / 10n; // 20 % headroom, like every wallet; unused gas is refunded
  }

  const explicitLegacy = req.gasPrice !== undefined || req.type === '0x0' || req.type === '0x1';
  let baseFee: bigint | null = null;
  if (!explicitLegacy) {
    const block = (await rpc('eth_getBlockByNumber', ['latest', false])) as { baseFeePerGas?: string } | null;
    baseFee = block?.baseFeePerGas ? asBigInt(block.baseFeePerGas, 'baseFeePerGas') : null;
  }

  const common = { chainId, nonce: Number(nonce), gas, to, value, data };

  if (explicitLegacy || baseFee === null) {
    const gasPrice = hexQty(req.gasPrice, 'gasPrice') ?? asBigInt(await rpc('eth_gasPrice', []), 'gasPrice');
    const tx: TransactionSerializableLegacy = { ...common, gasPrice, type: 'legacy' };
    return { unsigned: serializeTransaction(tx), type: 'legacy', ...common, gasPrice };
  }

  let maxPriorityFeePerGas = hexQty(req.maxPriorityFeePerGas, 'maxPriorityFeePerGas');
  if (maxPriorityFeePerGas === undefined) {
    try {
      maxPriorityFeePerGas = asBigInt(await rpc('eth_maxPriorityFeePerGas', []), 'maxPriorityFeePerGas');
    } catch {
      maxPriorityFeePerGas = GWEI; // node without the method: 1 gwei tip
    }
  }
  const maxFeePerGas = hexQty(req.maxFeePerGas, 'maxFeePerGas') ?? baseFee * 2n + maxPriorityFeePerGas;
  if (maxFeePerGas < maxPriorityFeePerGas) throw new EvmTxError('BAD_REQUEST', 'maxFeePerGas is below maxPriorityFeePerGas');
  const tx: TransactionSerializableEIP1559 = {
    ...common,
    type: 'eip1559',
    maxFeePerGas,
    maxPriorityFeePerGas,
    accessList,
  };
  return { unsigned: serializeTransaction(tx), type: 'eip1559', ...common, maxFeePerGas, maxPriorityFeePerGas };
}

/**
 * The daemon is untrusted by design: before broadcasting, check that what came back is a
 * signed transaction for the same chain with the same body we asked it to sign.
 */
export function checkSignedEvmTransaction(signed: unknown, prepared: PreparedEvmTx): Hex {
  if (typeof signed !== 'string' || !isHex(signed, { strict: true })) throw new EvmTxError('INTERNAL', 'daemon returned no signed tx');
  let parsed: ReturnType<typeof parseTransaction>;
  try {
    parsed = parseTransaction(signed);
  } catch (e) {
    throw new EvmTxError('INTERNAL', `daemon returned an unparseable tx: ${(e as Error).message}`);
  }
  const p = parsed as { chainId?: number; nonce?: number; to?: string | null; value?: bigint; data?: Hex; r?: Hex; s?: Hex };
  if (!p.r || !p.s) throw new EvmTxError('INTERNAL', 'daemon returned an unsigned tx');
  if (p.chainId !== prepared.chainId) throw new EvmTxError('INTERNAL', 'daemon signed for a different chain');
  if (p.nonce !== prepared.nonce) throw new EvmTxError('INTERNAL', 'daemon changed the nonce');
  const to = p.to ?? null;
  if ((to === null) !== (prepared.to === null) || (to && prepared.to && !sameAddress(to, prepared.to))) {
    throw new EvmTxError('INTERNAL', 'daemon changed the recipient');
  }
  if ((p.value ?? 0n) !== prepared.value) throw new EvmTxError('INTERNAL', 'daemon changed the value');
  if ((p.data ?? '0x') !== prepared.data) throw new EvmTxError('INTERNAL', 'daemon changed the calldata');
  return signed;
}
