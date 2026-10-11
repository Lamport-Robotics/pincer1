import { describe, expect, it } from 'vitest';
import bs58 from 'bs58';
import { PincerWallet } from '../src/inpage';
import { PageTransport } from '../src/lib/pageTransport';
import { PAGE_CHANNEL, isPageRequest, type PageRequest } from '../src/protocol';
import { fromBase64, toBase64 } from '../src/lib/bytes';
import { FakeWindow } from './fakes';

const PUBKEY = bs58.encode(new Uint8Array(32).fill(7));

/** A fake background that answers every page request on the window. */
function fakeBackend(win: FakeWindow, seen: PageRequest[]): void {
  win.addEventListener('message', (ev) => {
    if (ev.source !== win || !isPageRequest(ev.data)) return;
    const req = ev.data;
    seen.push(req);
    const answer = (result: unknown): void =>
      win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', id: req.id, ok: true, result }, win.location.origin);
    switch (req.method) {
      case 'connect':
        return answer({ pubkey: PUBKEY });
      case 'disconnect':
        return answer({});
      case 'signTransaction': {
        const tx = fromBase64((req.params as { tx: string }).tx);
        const signed = new Uint8Array(tx.length + 1);
        signed.set(tx, 1);
        signed[0] = 0xff;
        return answer({ tx: toBase64(signed) });
      }
      case 'signMessage':
        return answer({ signature: toBase64(new Uint8Array(64).fill(9)) });
      case 'signAndSendTransaction':
        return answer({ signature: bs58.encode(new Uint8Array(64).fill(3)) });
    }
  });
}

describe('PincerWallet (Wallet Standard surface)', () => {
  it('exposes the expected identity, chains and features', () => {
    const wallet = new PincerWallet(new PageTransport(new FakeWindow()));
    expect(wallet.name).toBe('Pincer1');
    expect(wallet.version).toBe('1.0.0');
    expect(wallet.icon.startsWith('data:image/png;base64,')).toBe(true);
    expect([...wallet.chains]).toEqual(['solana:mainnet', 'solana:devnet', 'solana:localnet']);
    expect(Object.keys(wallet.features).sort()).toEqual(
      [
        'solana:signAndSendTransaction',
        'solana:signMessage',
        'solana:signTransaction',
        'standard:connect',
        'standard:disconnect',
        'standard:events',
      ].sort(),
    );
    expect(wallet.accounts).toEqual([]);
  });

  it('connects, emits change, signs a transaction and a message, sends a transaction', async () => {
    const win = new FakeWindow('https://jup.ag');
    const seen: PageRequest[] = [];
    fakeBackend(win, seen);
    const wallet = new PincerWallet(new PageTransport(win));
    const changes: number[] = [];
    const off = wallet.features['standard:events'].on('change', (p) => changes.push(p.accounts?.length ?? -1));

    const { accounts } = await wallet.features['standard:connect'].connect();
    expect(accounts).toHaveLength(1);
    const account = accounts[0]!;
    expect(account.address).toBe(PUBKEY);
    expect(account.publicKey).toEqual(new Uint8Array(32).fill(7));
    expect(changes).toEqual([1]);

    const tx = new Uint8Array([1, 2, 3, 4]);
    const [signedTx] = await wallet.features['solana:signTransaction'].signTransaction({
      account,
      transaction: tx,
      chain: 'solana:mainnet',
    });
    expect(signedTx!.signedTransaction).toEqual(new Uint8Array([0xff, 1, 2, 3, 4]));
    expect(seen.at(-1)!.method).toBe('signTransaction');
    expect((seen.at(-1)!.params as { tx: string }).tx).toBe(toBase64(tx));

    const msg = new TextEncoder().encode('hello');
    const [signedMsg] = await wallet.features['solana:signMessage'].signMessage({ account, message: msg });
    expect(signedMsg!.signedMessage).toEqual(msg);
    expect(signedMsg!.signature).toEqual(new Uint8Array(64).fill(9));

    const [sent] = await wallet.features['solana:signAndSendTransaction'].signAndSendTransaction({
      account,
      transaction: tx,
      chain: 'solana:devnet',
      options: { skipPreflight: true },
    });
    expect(sent!.signature).toEqual(new Uint8Array(64).fill(3));
    expect(seen.at(-1)!.params).toMatchObject({ chain: 'solana:devnet', options: { skipPreflight: true } });

    off();
    await wallet.features['standard:disconnect'].disconnect();
    expect(wallet.accounts).toEqual([]);
    expect(changes).toEqual([1]); // listener removed before disconnect
  });

  it('refuses to sign for a foreign account or an unknown chain', async () => {
    const win = new FakeWindow();
    fakeBackend(win, []);
    const wallet = new PincerWallet(new PageTransport(win));
    const [account] = (await wallet.features['standard:connect'].connect()).accounts;
    const foreign = { ...account!, address: bs58.encode(new Uint8Array(32).fill(1)) };
    await expect(
      wallet.features['solana:signTransaction'].signTransaction({ account: foreign, transaction: new Uint8Array(1) }),
    ).rejects.toMatchObject({ code: 'BAD_REQUEST' });
    await expect(
      wallet.features['solana:signTransaction'].signTransaction({
        account: account!,
        transaction: new Uint8Array(1),
        chain: 'solana:testnet',
      }),
    ).rejects.toMatchObject({ code: 'BAD_REQUEST' });
  });

  it('drops the account when the extension reports a disconnect', async () => {
    const win = new FakeWindow();
    fakeBackend(win, []);
    const wallet = new PincerWallet(new PageTransport(win));
    await wallet.features['standard:connect'].connect();
    expect(wallet.accounts).toHaveLength(1);
    win.postMessage({ channel: PAGE_CHANNEL, dir: 'ext→page', event: 'disconnected' }, win.location.origin);
    await new Promise((r) => setTimeout(r, 0));
    expect(wallet.accounts).toEqual([]);
  });
});
