/**
 * BackgroundCore with the daemon's multi-chain payloads: fake daemon over MockSocket, fake EVM
 * JSON-RPC, fake bitcoin broadcaster. Checks the exact frames that go to the robot and that the
 * unsigned RLP we build decodes to what the dapp asked for.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { parseTransaction, serializeTransaction, type Hex } from 'viem';
import { BackgroundCore, CoreError } from '../src/background/core';
import { RpcError } from '../src/lib/jsonRpc';
import { PAGE_CHANNEL, type PageEvent, type PageRequest, type Settings } from '../src/protocol';
import { MemoryStorage, MockSocket, flush } from './fakes';

const ORIGIN = 'https://app.uniswap.org';
const SOL = 'PUBKEY1';
const EVM = '0x1111111111111111111111111111111111111111';
const BTC = 'bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4';
const TO = '0x2222222222222222222222222222222222222222';
const ADDRESSES = { solana: SOL, evm: EVM, bitcoin: BTC, bitcoinPublicKey: `02${'ab'.repeat(32)}` };

function req<M extends PageRequest['method']>(method: M, params: PageRequest<M>['params']): PageRequest<M> {
  return { channel: PAGE_CHANNEL, dir: 'page→ext', id: `req-${method}-${Math.random().toString(36).slice(2, 6)}`, method, params };
}

/** Canned EVM node: records calls, answers the fee/nonce/gas questions, accepts raw txs. */
function fakeEvmNode() {
  const calls: Array<{ url: string; method: string; params: unknown[] }> = [];
  const node = {
    calls,
    baseFee: '0x3b9aca00' as string | null, // 1 gwei
    failPriorityFee: false,
    rpc: vi.fn(async (url: string, method: string, params: unknown[]): Promise<unknown> => {
      calls.push({ url, method, params });
      switch (method) {
        case 'eth_getTransactionCount':
          return '0x7';
        case 'eth_estimateGas':
          return '0x5208'; // 21000
        case 'eth_getBlockByNumber':
          return node.baseFee ? { number: '0x100', baseFeePerGas: node.baseFee } : { number: '0x100' };
        case 'eth_maxPriorityFeePerGas':
          if (node.failPriorityFee) throw new RpcError(-32601, 'method not found');
          return '0x5f5e100'; // 0.1 gwei
        case 'eth_gasPrice':
          return '0x77359400'; // 2 gwei
        case 'eth_sendRawTransaction':
          return `0x${'aa'.repeat(32)}`;
        case 'eth_blockNumber':
          return '0x100';
        case 'eth_call':
          throw new RpcError(3, 'execution reverted', '0x08c379a0');
        default:
          throw new RpcError(-32601, `unexpected ${method}`);
      }
    }),
  };
  return node;
}

function harness(initial: Partial<Settings> = {}) {
  const storage = new MemoryStorage();
  Object.assign(storage.data, { approvedOrigins: [ORIGIN] }, initial);
  const sockets: MockSocket[] = [];
  const pageEvents: PageEvent[] = [];
  const node = fakeEvmNode();
  const broadcastBitcoin = vi.fn(async (_api: string, _hex: string) => 'f'.repeat(64));
  const applyTakeover = vi.fn(async (_on: boolean) => {});
  const core = new BackgroundCore({
    createSocket: (url) => {
      const s = new MockSocket(url);
      sockets.push(s);
      return s;
    },
    storage,
    setBadge: () => {},
    sendRawTransaction: async () => 'SIG',
    jsonRpc: node.rpc,
    broadcastBitcoin,
    applyTakeover,
  });
  core.onPageEvent((_name, ev) => pageEvents.push(ev));
  return { core, storage, sockets, node, broadcastBitcoin, applyTakeover, pageEvents, sock: () => sockets.at(-1)! };
}

/** Paired, socket open, origin connected with a daemon that reports all three addresses. */
async function connected(initial: Partial<Settings> = {}) {
  const h = harness({ token: 'tok-1', pubkey: SOL, ...initial });
  await h.core.init();
  h.sock().open();
  const c = h.core.handlePageRequest(ORIGIN, req('connect', {}));
  await flush();
  expect(h.sock().frames.at(-1)).toEqual({ t: 'connect', token: 'tok-1', origin: ORIGIN });
  h.sock().receive({ t: 'connected', pubkey: SOL, addresses: ADDRESSES });
  expect(await c).toEqual({ pubkey: SOL }); // the solana result shape is unchanged
  return h;
}

/** Make a "signed" RLP out of the unsigned one the daemon received (dummy r/s; nothing verifies them). */
function pretendSign(unsigned: Hex): Hex {
  const parsed = parseTransaction(unsigned);
  return serializeTransaction(parsed as never, { r: `0x${'11'.repeat(32)}`, s: `0x${'22'.repeat(32)}`, v: 27n, yParity: 0 }) as Hex;
}

describe('BackgroundCore · multi-chain', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('stores the addresses from `connected`, serves them to connectChain, and solana frames now carry chain:"solana"', async () => {
    const h = await connected();
    expect(h.storage.data.addresses).toEqual(ADDRESSES);
    expect(h.core.getState().addresses).toEqual(ADDRESSES);
    expect(h.core.getState().evmChainId).toBe(1);

    expect(await h.core.handlePageRequest(ORIGIN, req('connectChain', { chain: 'evm' }))).toEqual({ address: EVM, chainId: '0x1' });
    expect(await h.core.handlePageRequest(ORIGIN, req('connectChain', { chain: 'bitcoin' }))).toEqual({
      address: BTC,
      publicKey: ADDRESSES.bitcoinPublicKey,
      chainId: '0x1',
    });
    // one daemon connect served all three
    expect(h.sock().frames.filter((f) => f.t === 'connect')).toHaveLength(1);
    // another origin, silent → no prompt, no address
    expect(await h.core.handlePageRequest('https://other.example', req('connectChain', { chain: 'evm', silent: true }))).toEqual({
      address: null,
      chainId: '0x1',
    });
    expect(h.sock().frames.filter((f) => f.t === 'connect')).toHaveLength(1);

    const p = h.core.handlePageRequest(ORIGIN, req('signMessage', { message: 'aGk=' }));
    await flush();
    expect(h.sock().frames.at(-1)).toMatchObject({ t: 'signMessage', chain: 'solana', message: 'aGk=', origin: ORIGIN });
    h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, signature: 'c2ln' });
    expect(await p).toEqual({ signature: 'c2ln' });
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'signMessage', chain: 'solana' });
  });

  it('connectChain for a daemon without an EVM key is NOT_CONNECTED', async () => {
    const h = harness({ token: 'tok-1', pubkey: SOL });
    await h.core.init();
    h.sock().open();
    const c = h.core.handlePageRequest(ORIGIN, req('connectChain', { chain: 'evm' }));
    await flush();
    h.sock().receive({ t: 'connected', pubkey: SOL }); // old daemon: no addresses
    await expect(c).rejects.toMatchObject({ code: 'NOT_CONNECTED' });
    expect(h.core.getState().addresses).toEqual({ solana: SOL });
  });

  it('evm_sendTransaction: fills nonce/gas/fees from the configured RPC, sends unsigned type-2 RLP to the daemon, broadcasts the signed RLP', async () => {
    const h = await connected({ evmRpcUrls: { '1': 'https://my-node.example/rpc' } });
    const dappTx = { from: EVM, to: TO, value: '0xde0b6b3a7640000', data: '0xa9059cbb' };
    const sending = h.core.handlePageRequest(ORIGIN, req('evm_sendTransaction', { tx: dappTx }));
    await flush();

    // everything came from OUR rpc, in the user's chosen URL
    expect(h.node.calls.map((c) => c.method)).toEqual(['eth_getTransactionCount', 'eth_estimateGas', 'eth_getBlockByNumber', 'eth_maxPriorityFeePerGas']);
    expect(new Set(h.node.calls.map((c) => c.url))).toEqual(new Set(['https://my-node.example/rpc']));
    expect(h.node.calls[0]!.params).toEqual([EVM, 'pending']);
    expect(h.node.calls[1]!.params).toEqual([{ from: EVM, to: TO, value: '0xde0b6b3a7640000', data: '0xa9059cbb' }]);

    const frame = h.sock().frames.at(-1)!;
    expect(frame).toMatchObject({ t: 'signTransaction', chain: 'evm', chainId: 1, origin: ORIGIN });
    expect(h.core.getState().pending).toMatchObject({ kind: 'evm_sendTransaction', chain: 'evm', origin: ORIGIN });
    const unsigned = frame.tx as Hex;
    expect(unsigned.startsWith('0x02')).toBe(true);
    const decoded = parseTransaction(unsigned);
    expect(decoded).toMatchObject({
      type: 'eip1559',
      chainId: 1,
      nonce: 7,
      to: TO,
      value: 1_000_000_000_000_000_000n,
      data: '0xa9059cbb',
      gas: 25200n, // 21000 × 1.2
      maxPriorityFeePerGas: 100_000_000n,
      maxFeePerGas: 2_100_000_000n, // 2 × baseFee + tip
    });
    expect((decoded as { r?: Hex }).r).toBeUndefined(); // unsigned

    h.sock().receive({ t: 'explain', id: frame.id, summary: '1 ETH to 0x2222…', risk: 'medium', reasons: ['new recipient'], lines: ['One ether', 'to two-two-two-two'] });
    expect(h.core.getState().pending?.explain).toEqual({
      summary: '1 ETH to 0x2222…',
      risk: 'medium',
      reasons: ['new recipient'],
      lines: ['One ether', 'to two-two-two-two'],
    });

    const signed = pretendSign(unsigned);
    h.sock().receive({ t: 'signed', id: frame.id, tx: signed });
    expect(await sending).toEqual({ hash: `0x${'aa'.repeat(32)}` });
    expect(h.node.calls.at(-1)).toEqual({ url: 'https://my-node.example/rpc', method: 'eth_sendRawTransaction', params: [signed] });
    expect(h.core.getState().log[0]).toMatchObject({
      kind: 'evm_sendTransaction',
      chain: 'evm',
      outcome: 'signed',
      summary: '1 ETH to 0x2222…',
      detail: `sent 0xaaaaaaaa…`,
    });
  });

  it('evm_sendTransaction refuses a signed tx the daemon tampered with and never broadcasts it', async () => {
    const h = await connected();
    const sending = h.core.handlePageRequest(ORIGIN, req('evm_sendTransaction', { tx: { from: EVM, to: TO, value: '0x1' } })).catch((e) => e);
    await flush();
    const frame = h.sock().frames.at(-1)!;
    const tampered = serializeTransaction(
      { ...(parseTransaction(frame.tx as Hex) as object), to: '0x9999999999999999999999999999999999999999' } as never,
      { r: `0x${'11'.repeat(32)}`, s: `0x${'22'.repeat(32)}`, v: 28n, yParity: 1 },
    );
    h.sock().receive({ t: 'signed', id: frame.id, tx: tampered });
    const err = await sending;
    expect(err).toBeInstanceOf(CoreError);
    expect(err).toMatchObject({ code: 'INTERNAL', message: 'daemon changed the recipient' });
    expect(h.node.calls.some((c) => c.method === 'eth_sendRawTransaction')).toBe(false);
  });

  it('evm_signTransaction on a chain without a base fee builds an EIP-155 legacy tx and returns the signed RLP without sending', async () => {
    const h = await connected({ evmChainId: 56 });
    h.node.baseFee = null;
    const signing = h.core.handlePageRequest(ORIGIN, req('evm_signTransaction', { tx: { from: EVM, to: TO, value: '0x1', gas: '0x5208', nonce: '0x2' } }));
    await flush();
    expect(h.node.calls.map((c) => c.method)).toEqual(['eth_getBlockByNumber', 'eth_gasPrice']);
    expect(h.node.calls[0]!.url).toBe('https://bsc-dataseed.binance.org'); // chain 56 default RPC
    const frame = h.sock().frames.at(-1)!;
    expect(frame).toMatchObject({ chain: 'evm', chainId: 56 });
    const decoded = parseTransaction(frame.tx as Hex);
    expect(decoded).toMatchObject({ type: 'legacy', chainId: 56, nonce: 2, gas: 21000n, gasPrice: 2_000_000_000n, to: TO, value: 1n });
    const signed = pretendSign(frame.tx as Hex);
    h.sock().receive({ t: 'signed', id: frame.id, tx: signed });
    expect(await signing).toEqual({ tx: signed });
    expect(h.node.calls.some((c) => c.method === 'eth_sendRawTransaction')).toBe(false);
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'evm_signTransaction', chain: 'evm', outcome: 'signed' });
  });

  it('rejects an EVM tx from a foreign address or for another chain before touching the robot', async () => {
    const h = await connected();
    const before = h.sock().frames.length;
    await expect(
      h.core.handlePageRequest(ORIGIN, req('evm_sendTransaction', { tx: { from: TO, to: EVM } })),
    ).rejects.toMatchObject({ code: 'NOT_CONNECTED' });
    await expect(
      h.core.handlePageRequest(ORIGIN, req('evm_sendTransaction', { tx: { from: EVM, to: TO, chainId: '0x89' } })),
    ).rejects.toMatchObject({ code: 'BAD_REQUEST' });
    expect(h.sock().frames.length).toBe(before);
  });

  it('a daemon denial of an EVM request is DENIED with the reason and logged on chain evm', async () => {
    const h = await connected();
    const p = h.core.handlePageRequest(ORIGIN, req('evm_signMessage', { message: '0x68656c6c6f' })).catch((e) => e);
    await flush();
    const frame = h.sock().frames.at(-1)!;
    expect(frame).toMatchObject({ t: 'signMessage', chain: 'evm', message: '0x68656c6c6f', origin: ORIGIN });
    h.sock().receive({ t: 'denied', id: frame.id, reason: 'no touch' });
    expect(await p).toMatchObject({ code: 'DENIED', message: 'no touch' });
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'evm_signMessage', chain: 'evm', outcome: 'denied', detail: 'no touch' });
  });

  it('evm_signMessage / evm_signTypedData forward to the daemon and return its 0x signature', async () => {
    const h = await connected();
    const sig = `0x${'ab'.repeat(65)}`;
    const m = h.core.handlePageRequest(ORIGIN, req('evm_signMessage', { message: '0xdeadbeef' }));
    await flush();
    h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, signature: sig });
    expect(await m).toEqual({ signature: sig });

    const typedData = {
      types: { EIP712Domain: [{ name: 'chainId', type: 'uint256' }], Mail: [{ name: 'contents', type: 'string' }] },
      primaryType: 'Mail',
      domain: { chainId: 1 },
      message: { contents: 'hi' },
    };
    const t = h.core.handlePageRequest(ORIGIN, req('evm_signTypedData', { typedData }));
    await flush();
    expect(h.sock().frames.at(-1)).toMatchObject({ t: 'signMessage', chain: 'evm', typedData, origin: ORIGIN });
    expect(h.sock().frames.at(-1)!.message).toBeUndefined();
    h.sock().receive({ t: 'signed', id: h.sock().frames.at(-1)!.id, signature: sig });
    expect(await t).toEqual({ signature: sig });

    // typed data pinned to another chain is refused locally
    await expect(
      h.core.handlePageRequest(ORIGIN, req('evm_signTypedData', { typedData: { ...typedData, domain: { chainId: 137 } } })),
    ).rejects.toMatchObject({ code: 'BAD_REQUEST' });
    await expect(h.core.handlePageRequest(ORIGIN, req('evm_signMessage', { message: 'hello' }))).rejects.toMatchObject({ code: 'BAD_REQUEST' });
  });

  it('evm_switchChain: known chain persists, notifies pages, changes the RPC; unknown is UNSUPPORTED_CHAIN', async () => {
    const h = await connected();
    expect(await h.core.handlePageRequest(ORIGIN, req('evm_switchChain', { chainId: '0x2105' }))).toEqual({ chainId: '0x2105' });
    expect(h.pageEvents).toEqual([{ event: 'evmChainChanged', chainId: '0x2105' }]);
    expect(h.storage.data.evmChainId).toBe(8453);
    expect(h.core.getState().evmChainId).toBe(8453);
    expect(await h.core.handlePageRequest(ORIGIN, req('evm_chain', {}))).toEqual({ chainId: '0x2105' });
    expect(await h.core.handlePageRequest(ORIGIN, req('connectChain', { chain: 'evm', silent: true }))).toEqual({ address: EVM, chainId: '0x2105' });

    await expect(h.core.handlePageRequest(ORIGIN, req('evm_switchChain', { chainId: '0xfa' }))).rejects.toMatchObject({ code: 'UNSUPPORTED_CHAIN' });
    expect(h.pageEvents).toHaveLength(1);

    await h.core.handlePageRequest(ORIGIN, req('evm_rpc', { method: 'eth_blockNumber', params: [] }));
    expect(h.node.calls.at(-1)!.url).toBe('https://mainnet.base.org');

    // the popup can override the RPC per chain and reset it with an empty string
    await h.core.setEvmRpcUrl(8453, 'https://base.example/rpc');
    expect(h.core.getState().evmRpcUrls['8453']).toBe('https://base.example/rpc');
    await h.core.setEvmRpcUrl(8453, '');
    expect(h.core.getState().evmRpcUrls['8453']).toBe('https://mainnet.base.org');
    await expect(h.core.setEvmRpcUrl(8453, 'ftp://x')).rejects.toMatchObject({ code: 'BAD_REQUEST' });
  });

  it('evm_rpc proxies only read-only methods and carries node errors through as RPC_ERROR', async () => {
    const h = await connected();
    expect(await h.core.handlePageRequest(ORIGIN, req('evm_rpc', { method: 'eth_blockNumber', params: [] }))).toEqual({ result: '0x100' });
    const err = await h.core.handlePageRequest(ORIGIN, req('evm_rpc', { method: 'eth_call', params: [{}, 'latest'] })).catch((e) => e);
    expect(err).toMatchObject({ code: 'RPC_ERROR', message: 'execution reverted', rpc: { code: 3, data: '0x08c379a0' } });
    expect((err as CoreError).toJSON()).toEqual({ code: 'RPC_ERROR', message: 'execution reverted', rpc: { code: 3, data: '0x08c379a0' } });
    await expect(h.core.handlePageRequest(ORIGIN, req('evm_rpc', { method: 'eth_sendTransaction', params: [] }))).rejects.toMatchObject({
      code: 'UNSUPPORTED_METHOD',
    });
    await expect(h.core.handlePageRequest(ORIGIN, req('evm_rpc', { method: 'admin_addPeer', params: [] }))).rejects.toMatchObject({
      code: 'UNSUPPORTED_METHOD',
    });
  });

  it('btc_signPsbt sends {chain:"bitcoin", psbt, signInputs, finalize} and returns the signed PSBT; with broadcast it posts the finalized tx', async () => {
    const h = await connected({ btcApiUrl: 'https://mempool.example/api' });
    const psbt = 'cHNidP8BAHECAAAAAQ==';
    const p1 = h.core.handlePageRequest(ORIGIN, req('btc_signPsbt', { psbt, signInputs: { [BTC]: [0] } }));
    await flush();
    let frame = h.sock().frames.at(-1)!;
    expect(frame).toEqual({ t: 'signTransaction', id: frame.id, chain: 'bitcoin', psbt, signInputs: { [BTC]: [0] }, finalize: false, origin: ORIGIN });
    expect(h.core.getState().pending).toMatchObject({ kind: 'btc_signPsbt', chain: 'bitcoin' });
    h.sock().receive({ t: 'signed', id: frame.id, psbt: `${psbt}signed` });
    expect(await p1).toEqual({ psbt: `${psbt}signed` });
    expect(h.broadcastBitcoin).not.toHaveBeenCalled();

    const p2 = h.core.handlePageRequest(ORIGIN, req('btc_signPsbt', { psbt, broadcast: true }));
    await flush();
    frame = h.sock().frames.at(-1)!;
    expect(frame).toMatchObject({ chain: 'bitcoin', finalize: true });
    expect(frame.signInputs).toBeUndefined();
    h.sock().receive({ t: 'signed', id: frame.id, psbt: `${psbt}signed`, tx: '0200000001abcd' });
    expect(await p2).toEqual({ psbt: `${psbt}signed`, txid: 'f'.repeat(64) });
    expect(h.broadcastBitcoin).toHaveBeenCalledWith('https://mempool.example/api', '0200000001abcd');
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'btc_signPsbt', chain: 'bitcoin', outcome: 'signed', detail: 'sent ffffffffff…' });

    // broadcast asked for but the daemon did not finalize → SEND_FAILED, nothing posted
    const p3 = h.core.handlePageRequest(ORIGIN, req('btc_signPsbt', { psbt, broadcast: true })).catch((e) => e);
    await flush();
    frame = h.sock().frames.at(-1)!;
    h.sock().receive({ t: 'signed', id: frame.id, psbt: `${psbt}signed` });
    expect(await p3).toMatchObject({ code: 'SEND_FAILED' });
    expect(h.broadcastBitcoin).toHaveBeenCalledTimes(1);
  });

  it('btc_signMessage sends {chain:"bitcoin", message, address} (BIP-322) and refuses a foreign address', async () => {
    const h = await connected();
    const p = h.core.handlePageRequest(ORIGIN, req('btc_signMessage', { address: BTC, message: 'Sign in' }));
    await flush();
    const frame = h.sock().frames.at(-1)!;
    expect(frame).toEqual({ t: 'signMessage', id: frame.id, chain: 'bitcoin', message: 'Sign in', address: BTC, origin: ORIGIN });
    h.sock().receive({ t: 'signed', id: frame.id, signature: 'QmlQMzIy' });
    expect(await p).toEqual({ signature: 'QmlQMzIy' });
    expect(h.core.getState().log[0]).toMatchObject({ kind: 'btc_signMessage', chain: 'bitcoin', outcome: 'signed' });
    await expect(h.core.handlePageRequest(ORIGIN, req('btc_signMessage', { address: 'bc1qother', message: 'x' }))).rejects.toMatchObject({
      code: 'NOT_CONNECTED',
    });
  });

  it('the window.ethereum takeover toggle is applied on init and on change, and persisted', async () => {
    const h = harness({ takeoverEthereum: true });
    await h.core.init();
    expect(h.applyTakeover).toHaveBeenCalledWith(true);
    expect(h.core.getState().takeoverEthereum).toBe(true);
    await h.core.setTakeoverEthereum(false);
    expect(h.applyTakeover).toHaveBeenLastCalledWith(false);
    expect(h.storage.data.takeoverEthereum).toBe(false);
    expect(h.core.getState().takeoverEthereum).toBe(false);

    const fresh = harness();
    await fresh.core.init();
    expect(fresh.core.getState().takeoverEthereum).toBe(false); // off by default
    expect(fresh.applyTakeover).toHaveBeenCalledWith(false);
  });

  it('forget wipes the addresses too', async () => {
    const h = await connected();
    await h.core.forget();
    expect(h.storage.data.addresses).toBeUndefined();
    expect(h.core.getState().addresses).toEqual({ solana: undefined });
    expect(h.pageEvents.at(-1)).toEqual({ event: 'disconnected' });
  });
});
