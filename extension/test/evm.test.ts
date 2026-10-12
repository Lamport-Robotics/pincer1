/**
 * EIP-1193 / EIP-6963 provider surface, driven against a fake background that answers page
 * frames on the same window (the content script + service worker are covered elsewhere).
 */
import { describe, expect, it } from 'vitest';
import { Pincer1EthereumProvider, ProviderRpcError, RPC_ERRORS, announceEip6963 } from '../src/evm/provider';
import { installWallets, type Pincer1Namespace } from '../src/inpage';
import { PageTransport } from '../src/lib/pageTransport';
import { PAGE_CHANNEL, isPageRequest, type PageRequest, type WalletError } from '../src/protocol';
import { ICON_DATA_URI } from '../src/generated/icon';
import { FakeWindow, flush } from './fakes';

const EVM_ADDRESS = '0x1111111111111111111111111111111111111111';
const SIGNATURE = `0x${'ab'.repeat(65)}`;

interface BackendOptions {
  deny?: Set<string>;
  chainId?: string;
  known?: Set<string>;
}

/** A fake background: records every frame, answers like BackgroundCore would. */
function fakeBackend(win: FakeWindow, seen: PageRequest[], opts: BackendOptions = {}) {
  const state = { chainId: opts.chainId ?? '0x1', connected: false };
  const known = opts.known ?? new Set(['0x1', '0x2105', '0xa4b1', '0x38', '0x89']);
  win.addEventListener('message', (ev) => {
    if (ev.source !== win || !isPageRequest(ev.data)) return;
    const req = ev.data;
    seen.push(req);
    const ok = (result: unknown): void =>
      win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', id: req.id, ok: true, result }, win.location.origin);
    const fail = (error: WalletError): void =>
      win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', id: req.id, ok: false, error }, win.location.origin);
    if (opts.deny?.has(req.method)) return fail({ code: 'DENIED', message: 'robot said no' });
    switch (req.method) {
      case 'connectChain': {
        const p = req.params as { chain: string; silent?: boolean };
        if (p.silent && !state.connected) return ok({ address: null, chainId: state.chainId });
        state.connected = true;
        return ok({ address: EVM_ADDRESS, chainId: state.chainId });
      }
      case 'disconnect':
        state.connected = false;
        return ok({});
      case 'evm_chain':
        return ok({ chainId: state.chainId });
      case 'evm_switchChain': {
        const { chainId } = req.params as { chainId: string };
        if (!known.has(chainId)) return fail({ code: 'UNSUPPORTED_CHAIN', message: `chain ${chainId} unknown` });
        state.chainId = chainId;
        // the real background broadcasts this to every page
        win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', event: 'evmChainChanged', chainId }, win.location.origin);
        return ok({ chainId });
      }
      case 'evm_rpc': {
        const { method } = req.params as { method: string };
        if (method === 'eth_blockNumber') return ok({ result: '0x10' });
        return fail({ code: 'RPC_ERROR', message: 'execution reverted', rpc: { code: 3, data: '0xdead' } });
      }
      case 'evm_sendTransaction':
        return ok({ hash: `0x${'11'.repeat(32)}` });
      case 'evm_signTransaction':
        return ok({ tx: '0x02f8signed' });
      case 'evm_signMessage':
      case 'evm_signTypedData':
        return ok({ signature: SIGNATURE });
      default:
        return fail({ code: 'BAD_REQUEST', message: `unexpected ${req.method}` });
    }
  });
  return state;
}

function setup(opts: BackendOptions = {}) {
  const win = new FakeWindow('https://app.uniswap.org');
  const seen: PageRequest[] = [];
  const backend = fakeBackend(win, seen, opts);
  const provider = new Pincer1EthereumProvider(new PageTransport(win));
  return { win, seen, backend, provider };
}

const errorOf = (p: Promise<unknown>) => p.then(() => null, (e: unknown) => e as ProviderRpcError);

describe('EIP-6963 announcement', () => {
  it('dispatches eip6963:announceProvider with the Pincer1 info and re-announces on request', () => {
    const win = new FakeWindow();
    const provider = new Pincer1EthereumProvider(new PageTransport(win));
    const info = announceEip6963(win, provider, 'uuid-fixed');
    expect(info).toEqual({ uuid: 'uuid-fixed', name: 'Pincer1', icon: ICON_DATA_URI, rdns: 'com.lamportrobotics.pincer1' });
    expect(info.icon.startsWith('data:image/png;base64,')).toBe(true);

    expect(win.dispatched).toHaveLength(1);
    const ev = win.dispatched[0] as CustomEvent<{ info: typeof info; provider: unknown }>;
    expect(ev.type).toBe('eip6963:announceProvider');
    expect(ev.detail.info).toEqual(info);
    expect(ev.detail.provider).toBe(provider);
    expect(Object.isFrozen(ev.detail)).toBe(true);
    expect(Object.isFrozen(ev.detail.info)).toBe(true);

    // a dapp that loads later asks, and gets the same provider again
    win.dispatchEvent(new Event('eip6963:requestProvider'));
    expect(win.dispatched.filter((e) => e.type === 'eip6963:announceProvider')).toHaveLength(2);
    const again = win.dispatched.at(-1) as CustomEvent<{ info: typeof info; provider: unknown }>;
    expect(again.detail.info.uuid).toBe('uuid-fixed');
    expect(again.detail.provider).toBe(provider);
  });

  it('installWallets exposes window.pincer1.{ethereum,bitcoin}, announces EIP-6963, never touches window.ethereum', () => {
    const win = new FakeWindow() as FakeWindow & { pincer1?: Pincer1Namespace; ethereum?: unknown };
    const ns = installWallets(win, new PageTransport(win));
    expect(win.pincer1).toBe(ns);
    expect(ns.ethereum).toBeInstanceOf(Pincer1EthereumProvider);
    expect(ns.ethereum.isPincer1).toBe(true);
    expect((ns.ethereum as unknown as { isMetaMask?: boolean }).isMetaMask).toBeUndefined();
    expect(typeof ns.bitcoin.requestAccounts).toBe('function');
    expect(typeof ns.request).toBe('function');
    expect(win.dispatched.some((e) => e.type === 'eip6963:announceProvider')).toBe(true);
    expect(win.ethereum).toBeUndefined();
    expect((win.btc_providers as Array<{ id: string }>).map((p) => p.id)).toEqual(['pincer1']);
  });

  it('installWallets defines window.ethereum only when the takeover flag was left by takeover.js', () => {
    const win = new FakeWindow() as FakeWindow & { __pincer1TakeoverEthereum?: boolean; ethereum?: unknown };
    win.__pincer1TakeoverEthereum = true;
    const ns = installWallets(win, new PageTransport(win));
    expect(win.ethereum).toBe(ns.ethereum);
  });
});

describe('Pincer1EthereumProvider', () => {
  it('eth_requestAccounts connects through the background, emits connect + accountsChanged, eth_chainId reads the chain', async () => {
    const { provider, seen } = setup({ chainId: '0x2105' });
    const events: Array<[string, unknown]> = [];
    provider.on('connect', (e) => events.push(['connect', e]));
    provider.on('accountsChanged', (a) => events.push(['accountsChanged', a]));

    expect(provider.isConnected()).toBe(false);
    expect(await provider.request({ method: 'eth_accounts' })).toEqual([]); // silent, not connected yet
    expect(seen.at(-1)!.params).toEqual({ chain: 'evm', silent: true });

    const accounts = await provider.request({ method: 'eth_requestAccounts' });
    expect(accounts).toEqual([EVM_ADDRESS]);
    expect(seen.at(-1)!.method).toBe('connectChain');
    expect(seen.at(-1)!.params).toEqual({ chain: 'evm' });
    expect(events).toEqual([
      ['connect', { chainId: '0x2105' }],
      ['accountsChanged', [EVM_ADDRESS]],
    ]);
    expect(provider.isConnected()).toBe(true);
    expect(provider.selectedAddress).toBe(EVM_ADDRESS);
    expect(await provider.request({ method: 'eth_accounts' })).toEqual([EVM_ADDRESS]);
    expect(await provider.request({ method: 'eth_chainId' })).toBe('0x2105');
    expect(provider.chainId).toBe('0x2105');
    expect(await provider.request({ method: 'net_version' })).toBe('8453');
    expect(provider.networkVersion).toBe('8453');
  });

  it('eth_chainId works before any account is connected and does not prompt', async () => {
    const { provider, seen } = setup();
    expect(await provider.request({ method: 'eth_chainId' })).toBe('0x1');
    expect(seen.map((r) => r.method)).toEqual(['evm_chain']);
  });

  it('eth_sendTransaction forwards the dapp tx and returns the hash; eth_signTransaction returns the signed RLP', async () => {
    const { provider, seen } = setup();
    await provider.request({ method: 'eth_requestAccounts' });
    const tx = { from: EVM_ADDRESS, to: '0x2222222222222222222222222222222222222222', value: '0xde0b6b3a7640000', data: '0x' };
    const hash = await provider.request({ method: 'eth_sendTransaction', params: [tx] });
    expect(hash).toBe(`0x${'11'.repeat(32)}`);
    expect(seen.at(-1)!.method).toBe('evm_sendTransaction');
    expect(seen.at(-1)!.params).toEqual({ tx });

    expect(await provider.request({ method: 'eth_signTransaction', params: [tx] })).toBe('0x02f8signed');
    expect(seen.at(-1)!.method).toBe('evm_signTransaction');
  });

  it('eth_sendTransaction from a foreign address is 4100 and never reaches the background', async () => {
    const { provider, seen } = setup();
    await provider.request({ method: 'eth_requestAccounts' });
    const before = seen.length;
    const err = await errorOf(
      provider.request({ method: 'eth_sendTransaction', params: [{ from: '0x9999999999999999999999999999999999999999', to: EVM_ADDRESS }] }),
    );
    expect(err).toBeInstanceOf(ProviderRpcError);
    expect(err!.code).toBe(RPC_ERRORS.UNAUTHORIZED);
    expect(seen.length).toBe(before);
  });

  it('personal_sign forwards hex as-is and UTF-8 as hex, accepting both argument orders', async () => {
    const { provider, seen } = setup();
    await provider.request({ method: 'eth_requestAccounts' });

    expect(await provider.request({ method: 'personal_sign', params: ['0xdeadbeef', EVM_ADDRESS] })).toBe(SIGNATURE);
    expect(seen.at(-1)!.method).toBe('evm_signMessage');
    expect(seen.at(-1)!.params).toEqual({ message: '0xdeadbeef' });

    await provider.request({ method: 'personal_sign', params: [EVM_ADDRESS, 'hello'] });
    expect(seen.at(-1)!.params).toEqual({ message: '0x68656c6c6f' });
  });

  it('eth_signTypedData_v4 parses the JSON string and forwards the typed data object', async () => {
    const { provider, seen } = setup();
    await provider.request({ method: 'eth_requestAccounts' });
    const typedData = {
      types: {
        EIP712Domain: [{ name: 'name', type: 'string' }, { name: 'chainId', type: 'uint256' }],
        Permit: [{ name: 'owner', type: 'address' }, { name: 'value', type: 'uint256' }],
      },
      primaryType: 'Permit',
      domain: { name: 'USD Coin', chainId: 1 },
      message: { owner: EVM_ADDRESS, value: '1000000' },
    };
    const sig = await provider.request({ method: 'eth_signTypedData_v4', params: [EVM_ADDRESS, JSON.stringify(typedData)] });
    expect(sig).toBe(SIGNATURE);
    expect(seen.at(-1)!.method).toBe('evm_signTypedData');
    expect(seen.at(-1)!.params).toEqual({ typedData });

    // already-parsed objects are fine too
    await provider.request({ method: 'eth_signTypedData_v4', params: [EVM_ADDRESS, typedData] });
    expect(seen.at(-1)!.params).toEqual({ typedData });

    const bad = await errorOf(provider.request({ method: 'eth_signTypedData_v4', params: [EVM_ADDRESS, '{not json'] }));
    expect(bad!.code).toBe(RPC_ERRORS.INVALID_PARAMS);
  });

  it('eth_sign and legacy eth_signTypedData (v1) are refused with 4200', async () => {
    const { provider, seen } = setup();
    await provider.request({ method: 'eth_requestAccounts' });
    const before = seen.length;
    const e1 = await errorOf(provider.request({ method: 'eth_sign', params: [EVM_ADDRESS, '0xdeadbeef'] }));
    expect(e1!.code).toBe(RPC_ERRORS.UNSUPPORTED_METHOD);
    const e2 = await errorOf(provider.request({ method: 'eth_signTypedData', params: [[], EVM_ADDRESS] }));
    expect(e2!.code).toBe(RPC_ERRORS.UNSUPPORTED_METHOD);
    const e3 = await errorOf(provider.request({ method: 'debug_traceTransaction', params: [] }));
    expect(e3!.code).toBe(RPC_ERRORS.UNSUPPORTED_METHOD);
    expect(seen.length).toBe(before);
  });

  it('maps a daemon denial to 4001 and a missing account to 4100', async () => {
    const denied = setup({ deny: new Set(['evm_signMessage', 'connectChain']) });
    const e1 = await errorOf(denied.provider.request({ method: 'eth_requestAccounts' }));
    expect(e1!.code).toBe(RPC_ERRORS.USER_REJECTED);
    expect(e1!.message).toBe('robot said no');

    const { provider } = setup({ deny: new Set(['evm_signMessage']) });
    const e2 = await errorOf(provider.request({ method: 'personal_sign', params: ['0x01', EVM_ADDRESS] }));
    expect(e2!.code).toBe(RPC_ERRORS.UNAUTHORIZED); // nothing connected yet
    await provider.request({ method: 'eth_requestAccounts' });
    const e3 = await errorOf(provider.request({ method: 'personal_sign', params: ['0x01', EVM_ADDRESS] }));
    expect(e3!.code).toBe(RPC_ERRORS.USER_REJECTED);
  });

  it('wallet_switchEthereumChain: known chain switches and emits chainChanged; unknown is 4902; addEthereumChain for a known chain is a switch', async () => {
    const { provider, backend } = setup();
    const changes: unknown[] = [];
    provider.on('chainChanged', (c) => changes.push(c));
    await provider.request({ method: 'eth_chainId' });

    expect(await provider.request({ method: 'wallet_switchEthereumChain', params: [{ chainId: '0xa4b1' }] })).toBeNull();
    await flush();
    expect(backend.chainId).toBe('0xa4b1');
    expect(provider.chainId).toBe('0xa4b1');
    expect(changes).toEqual(['0xa4b1']);

    const unknown = await errorOf(provider.request({ method: 'wallet_switchEthereumChain', params: [{ chainId: '0xfa' }] }));
    expect(unknown!.code).toBe(RPC_ERRORS.UNRECOGNIZED_CHAIN);
    expect(provider.chainId).toBe('0xa4b1');

    expect(
      await provider.request({
        method: 'wallet_addEthereumChain',
        params: [{ chainId: '0x89', chainName: 'Polygon', rpcUrls: ['https://evil.example/rpc'] }],
      }),
    ).toBeNull();
    await flush();
    expect(backend.chainId).toBe('0x89');
    expect(changes).toEqual(['0xa4b1', '0x89']);

    const junk = await errorOf(provider.request({ method: 'wallet_switchEthereumChain', params: [{ chainId: 'mainnet' }] }));
    expect(junk!.code).toBe(RPC_ERRORS.INVALID_PARAMS);
  });

  it('proxies read-only methods and passes RPC errors through with their code and data', async () => {
    const { provider, seen } = setup();
    expect(await provider.request({ method: 'eth_blockNumber', params: [] })).toBe('0x10');
    expect(seen.at(-1)!.method).toBe('evm_rpc');
    expect(seen.at(-1)!.params).toEqual({ method: 'eth_blockNumber', params: [] });
    const err = await errorOf(provider.request({ method: 'eth_call', params: [{ to: EVM_ADDRESS }, 'latest'] }));
    expect(err!.code).toBe(3);
    expect(err!.data).toBe('0xdead');
    expect(err!.message).toBe('execution reverted');
  });

  it('drops the account and emits disconnect when the extension is unpaired', async () => {
    const { provider, win } = setup();
    await provider.request({ method: 'eth_requestAccounts' });
    const events: string[] = [];
    provider.on('accountsChanged', (a) => events.push(`accounts:${(a as string[]).length}`));
    provider.on('disconnect', (e) => events.push(`disconnect:${(e as ProviderRpcError).code}`));
    win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', event: 'disconnected' }, win.location.origin);
    await flush();
    expect(events).toEqual(['accounts:0', 'disconnect:4900']);
    expect(provider.selectedAddress).toBeNull();
    expect(provider.isConnected()).toBe(false);
  });
});
