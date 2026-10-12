/** Bitcoin provider surface against a fake background. */
import { describe, expect, it } from 'vitest';
import { BTC_RPC_ERRORS, BitcoinProviderError, Pincer1BitcoinProvider, registerBtcProvider } from '../src/bitcoin/provider';
import { PageTransport } from '../src/lib/pageTransport';
import { PAGE_CHANNEL, isPageRequest, type PageRequest, type WalletError } from '../src/protocol';
import { FakeWindow } from './fakes';

const ADDRESS = 'bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4';
const PUBKEY = `02${'ab'.repeat(32)}`;
const PSBT = 'cHNidP8BAHECAAAAAQ=='; // any base64 will do: the extension never parses PSBTs
const TXID = 'f'.repeat(64);

function fakeBackend(win: FakeWindow, seen: PageRequest[], deny = new Set<string>()) {
  const state = { connected: false };
  win.addEventListener('message', (ev) => {
    if (ev.source !== win || !isPageRequest(ev.data)) return;
    const req = ev.data;
    seen.push(req);
    const ok = (result: unknown): void =>
      win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', id: req.id, ok: true, result }, win.location.origin);
    const fail = (error: WalletError): void =>
      win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', id: req.id, ok: false, error }, win.location.origin);
    if (deny.has(req.method)) return fail({ code: 'DENIED', message: 'robot said no' });
    switch (req.method) {
      case 'connectChain': {
        const p = req.params as { chain: string; silent?: boolean };
        if (p.silent && !state.connected) return ok({ address: null, chainId: '0x1' });
        state.connected = true;
        return ok({ address: ADDRESS, publicKey: PUBKEY, chainId: '0x1' });
      }
      case 'btc_signPsbt': {
        const p = req.params as { psbt: string; broadcast?: boolean };
        return ok(p.broadcast ? { psbt: `${p.psbt}signed`, txid: TXID } : { psbt: `${p.psbt}signed` });
      }
      case 'btc_signMessage':
        return ok({ signature: 'QmlQMzIy' });
      default:
        return fail({ code: 'BAD_REQUEST', message: `unexpected ${req.method}` });
    }
  });
}

function setup(deny?: Set<string>) {
  const win = new FakeWindow('https://magiceden.io');
  const seen: PageRequest[] = [];
  fakeBackend(win, seen, deny);
  const provider = new Pincer1BitcoinProvider(new PageTransport(win));
  return { win, seen, provider };
}

const errorOf = (p: Promise<unknown>) => p.then(() => null, (e: unknown) => e as BitcoinProviderError);

describe('Pincer1BitcoinProvider', () => {
  it('requestAccounts returns one p2wpkh payment account; getAccounts is silent until connected', async () => {
    const { provider, seen } = setup();
    expect(await provider.getAccounts()).toEqual([]);
    expect(seen.at(-1)!.params).toEqual({ chain: 'bitcoin', silent: true });

    const accounts = await provider.requestAccounts();
    expect(accounts).toEqual([{ address: ADDRESS, publicKey: PUBKEY, purpose: 'payment', addressType: 'p2wpkh' }]);
    expect(seen.at(-1)!.method).toBe('connectChain');
    expect(seen.at(-1)!.params).toEqual({ chain: 'bitcoin' });
    expect(await provider.getAccounts()).toEqual(accounts);
    expect(await provider.getNetwork()).toEqual({ bitcoin: { name: 'Mainnet' }, stacks: { name: 'Mainnet' } });
  });

  it('signPsbt forwards psbt + signInputs + broadcast and returns the signed PSBT (and txid when broadcast)', async () => {
    const { provider, seen } = setup();
    await provider.requestAccounts();
    const r1 = await provider.signPsbt(PSBT, { signInputs: { [ADDRESS]: [0, 1] } });
    expect(r1).toEqual({ psbt: `${PSBT}signed` });
    expect(seen.at(-1)!.method).toBe('btc_signPsbt');
    expect(seen.at(-1)!.params).toEqual({ psbt: PSBT, signInputs: { [ADDRESS]: [0, 1] }, broadcast: false });

    const r2 = await provider.signPsbt(PSBT, { broadcast: true });
    expect(r2).toEqual({ psbt: `${PSBT}signed`, txid: TXID });
    expect(seen.at(-1)!.params).toMatchObject({ broadcast: true });

    const foreign = await errorOf(provider.signPsbt(PSBT, { signInputs: { bc1qother: [0] } }));
    expect(foreign!.code).toBe(BTC_RPC_ERRORS.INVALID_PARAMS);
    const notB64 = await errorOf(provider.signPsbt('not base64!'));
    expect(notB64!.code).toBe(BTC_RPC_ERRORS.INVALID_PARAMS);
  });

  it('signMessage forwards address + message (BIP-322) and refuses before connect / for another address', async () => {
    const { provider, seen } = setup();
    const early = await errorOf(provider.signMessage(ADDRESS, 'hi'));
    expect(early!.code).toBe(BTC_RPC_ERRORS.ACCESS_DENIED);
    await provider.requestAccounts();
    expect(await provider.signMessage(ADDRESS, 'Sign in to Magic Eden')).toBe('QmlQMzIy');
    expect(seen.at(-1)!.method).toBe('btc_signMessage');
    expect(seen.at(-1)!.params).toEqual({ address: ADDRESS, message: 'Sign in to Magic Eden' });
    const other = await errorOf(provider.signMessage('bc1qother', 'hi'));
    expect(other!.code).toBe(BTC_RPC_ERRORS.INVALID_PARAMS);
  });

  it('a daemon denial surfaces as USER_REJECTION (-32000) in both call styles', async () => {
    const { provider } = setup(new Set(['btc_signPsbt']));
    await provider.requestAccounts();
    const err = await errorOf(provider.signPsbt(PSBT));
    expect(err!.code).toBe(BTC_RPC_ERRORS.USER_REJECTION);
    const res = await provider.request('signPsbt', { psbt: PSBT }, 'r1');
    expect(res).toEqual({ jsonrpc: '2.0', id: 'r1', error: { code: -32000, message: 'robot said no' } });
  });

  it('request() speaks the sats-connect envelope', async () => {
    const { provider } = setup();
    const addrs = await provider.request('getAddresses', undefined, 'a');
    expect(addrs).toEqual({
      jsonrpc: '2.0',
      id: 'a',
      result: { addresses: [{ address: ADDRESS, publicKey: PUBKEY, purpose: 'payment', addressType: 'p2wpkh' }] },
    });
    const net = await provider.request('wallet_getNetwork', undefined, 'n');
    expect(net).toMatchObject({ result: { bitcoin: { name: 'Mainnet' } } });
    const signed = await provider.request('signPsbt', { psbt: PSBT, signInputs: { [ADDRESS]: [0] } }, 's');
    expect(signed).toEqual({ jsonrpc: '2.0', id: 's', result: { psbt: `${PSBT}signed` } });
    const msg = await provider.request('signMessage', { address: ADDRESS, message: 'yo' }, 'm');
    expect(msg).toEqual({ jsonrpc: '2.0', id: 'm', result: { signature: 'QmlQMzIy', address: ADDRESS } });
    const nope = await provider.request('sendTransfer', {}, 'x');
    expect(nope).toMatchObject({ error: { code: BTC_RPC_ERRORS.METHOD_NOT_SUPPORTED } });
  });

  it('registers a WBIP004 entry on window.btc_providers once', () => {
    const win = { btc_providers: [{ id: 'XverseProviders.BitcoinProvider', name: 'Xverse' }] } as { btc_providers?: unknown };
    registerBtcProvider(win);
    registerBtcProvider(win);
    const list = win.btc_providers as Array<{ id: string; name: string; icon: string; methods: string[] }>;
    expect(list.map((p) => p.id)).toEqual(['XverseProviders.BitcoinProvider', 'pincer1']);
    expect(list[1]).toMatchObject({ name: 'Pincer1', methods: expect.arrayContaining(['signPsbt', 'signMessage', 'getAccounts']) });
    expect(list[1]!.icon.startsWith('data:image/png;base64,')).toBe(true);
    const empty = {} as { btc_providers?: unknown };
    registerBtcProvider(empty);
    expect((empty.btc_providers as unknown[]).length).toBe(1);
  });
});
