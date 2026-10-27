# Pincer1 browser extension

A Chrome (Manifest V3) extension that puts the robot's keys in front of dapps as **three wallets named Pincer1**:

| Chain   | How dapps find it                                                                                   | Account                 |
| ------- | --------------------------------------------------------------------------------------------------- | ----------------------- |
| Solana  | [Wallet Standard](https://github.com/wallet-standard/wallet-standard) `registerWallet` — listed by `@solana/wallet-adapter` next to Phantom | one ed25519 pubkey      |
| EVM     | [EIP-6963](https://eips.ethereum.org/EIPS/eip-6963) `eip6963:announceProvider` (rdns `com.lamportrobotics.pincer1`) + `window.pincer1.ethereum`; `window.ethereum` only if you opt in | one secp256k1 address, chains 1 · 8453 · 42161 · 56 · 137 |
| Bitcoin | `window.pincer1.bitcoin` + a WBIP004 entry in `window.btc_providers` (id `pincer1`)               | one P2WPKH address, mainnet |

The extension never holds a key. Every signature request is forwarded over **one WebSocket** to `pincerd`,
the daemon on the robot, which parses the transaction, has the robot read it out loud, and only then hands it
to the consent controller — the only component that can sign, and only after a physical touch.
See `../docs/PROTOCOL.md` §3 (and §3.1 for the per-chain payloads) for the exact wire contract.

```
dapp ──(Wallet Standard / EIP-1193 / btc)──▶ inpage.js ──postMessage──▶ content.js ──port──▶ background.js ──ws://robot:7401──▶ pincerd
 MAIN world (page)                            MAIN world                isolated world        service worker
                                                                                                    └──fetch──▶ your RPC (nonce/gas/broadcast)
```

## Build and load

Requires Node ≥ 22 (pinned to 24 in `.node-version`) and pnpm 10.

```sh
pnpm install
pnpm build        # → dist/  (a loadable unpacked extension)
pnpm test
pnpm typecheck    # tsc --noEmit
```

Load it:

1. Open `chrome://extensions`, turn on **Developer mode** (top right).
2. **Load unpacked** → pick the `dist/` folder.
3. Pin the Pincer1 action (puzzle-piece menu → pin) so the badge is visible.

After every `pnpm build`, hit the reload icon on the extension card; pages already open need a refresh to get
the new `inpage.js`.

## Pairing

1. Open the popup. The robot address defaults to `ws://pincer1.local:7401`; change it if your robot has a
   different host or IP and press **Save**. The header dot turns lime once the socket is open.
2. On the robot, start pairing. It speaks a 6-digit code.
3. Type the code into the popup and press **Pair**. The extension sends `{"t":"pair","code":"…"}` once and
   stores the returned token and pubkey in `chrome.storage.local`. The code is never stored.
4. The popup now shows the **three addresses** (click any to copy) and **Forget pairing**, which revokes the
   token on the robot, drops it and the addresses, forgets every site approval, and disconnects every site.
5. The first time a site calls connect, the popup asks **Approve / Deny** for that origin (deny = `4001`);
   approved sites are listed with a **Remove** button and never asked again until removed. The EVM and bitcoin addresses arrive with the daemon's
   `connected` reply, so they appear after the first dapp connect (or at pairing, if the daemon includes
   `addresses` in `paired`).

## How a request flows

- **Connect.** Any of the three wallets connecting runs the same handshake once per origin:
  `{"t":"connect","token":"…","origin":"https://pump.fun"}` → `{"t":"connected","pubkey":"…","addresses":{solana,evm,bitcoin}}`.
  The origin comes from the content-script port's `sender.origin`, not from the page, so a page cannot claim
  to be someone else. A site connected on Solana is connected on EVM and Bitcoin too (one robot, one
  consent); after a reconnect or a worker restart the handshake is re-run transparently before the next sign.
- **Explain, then touch.** For every sign the daemon first replies `{"t":"explain","id","summary","risk","reasons","lines?"}`.
  The popup card shows the **chain**, what the dapp asked for, the summary, the risk pill, the reasons and
  the lines the robot spoke, plus "waiting for touch on the robot…"; the action badge counts pending
  requests. Then `{"t":"signed",…}` resolves the dapp's promise or `{"t":"denied","reason"}` rejects it.
  Requests that get no answer in **90 s** fail with `TIMEOUT` (the daemon's own touch ttl is 60 s).
- **Log.** The last 20 requests (chain, origin, kind, summary, risk, outcome) are kept in
  `chrome.storage.local` and shown in the popup.

### Solana (unchanged)

`solana:signTransaction` → `{"t":"signTransaction","chain":"solana","tx":<base64>}`; `solana:signMessage` →
`{"t":"signMessage","chain":"solana","message":<base64>}`. `signAndSendTransaction` signs through the robot
and broadcasts with `@solana/web3.js` to the Solana RPC URL from the popup (default
`https://api.mainnet-beta.solana.com`; devnet/localnet requests always go to their public endpoints).

### EVM (`src/evm/provider.ts`)

An EIP-1193 provider. What each method does:

| Method | What happens |
| --- | --- |
| `eth_requestAccounts`, `wallet_requestPermissions` | daemon `connect` for this origin → `[address]`; emits `connect {chainId}` and `accountsChanged` |
| `eth_accounts` | `[address]` if this origin is already connected, else `[]` — never prompts |
| `eth_chainId`, `net_version` | the wallet's current chain (persisted, shared by all tabs) |
| `wallet_switchEthereumChain` | known chain (1, 8453, 42161, 56, 137) → switches, every open tab gets `chainChanged`; unknown → **4902** |
| `wallet_addEthereumChain` | known chain → treated as a switch (the dapp's `rpcUrls` are ignored on purpose); unknown → 4902 |
| `eth_sendTransaction` | background validates `from`, fills **nonce / gas / fees from the RPC you configured for that chain** (never the dapp's node), serializes the unsigned tx with viem (type-2, or EIP-155 legacy when `gasPrice` is given or the chain has no base fee), sends `{"t":"signTransaction","chain":"evm","tx":"0x…","chainId"}`, re-parses the signed RLP and refuses it if chainId/nonce/to/value/data changed, then `eth_sendRawTransaction` on the same RPC → tx hash |
| `eth_signTransaction` | same, returns the signed RLP instead of broadcasting |
| `personal_sign` | `{"t":"signMessage","chain":"evm","message":"0x…"}` (hex as-is, UTF-8 encoded otherwise; both `[data, address]` and `[address, data]` orders accepted) |
| `eth_signTypedData_v4` (and `_v3`) | `{"t":"signMessage","chain":"evm","typedData":{…}}`; typed data whose `domain.chainId` is not the current chain is refused locally |
| `eth_sign`, `eth_signTypedData` (v1), `eth_decrypt`, `wallet_watchAsset` | **4200** unsupported |
| read-only `eth_*` (call, estimateGas, getBalance, getLogs, blockNumber, feeHistory, …), `web3_clientVersion` | proxied to the chain's RPC; node errors come back with the node's code and `data` |
| anything else (`debug_*`, `admin_*`, …) | 4200 — the wallet is not an open proxy |

Error codes: **4001** the robot denied (`error.message` is the daemon's reason), **4100** not connected / `from`
is not the Pincer1 account / the robot has no EVM key, **4200** unsupported method, **4900** robot offline,
**4902** unknown chain, **-32602** malformed params, **-32603** anything else.

Events: `connect`, `disconnect` (after *Forget pairing*), `accountsChanged`, `chainChanged`. Legacy
properties `chainId`, `networkVersion`, `selectedAddress`, `isConnected()`, `enable()` exist for older dapps.
There is deliberately no `isMetaMask`.

**`window.ethereum` takeover (off by default).** With the toggle off, `window.ethereum` is never touched:
dapps find Pincer1 through EIP-6963 (wagmi ≥ 1.4, RainbowKit, Web3Modal/Reown, viem's `createWalletClient`
with the announced provider, MetaMask SDK's picker, …) or `window.pincer1.ethereum`. Turning it on makes the
background register `takeover.js` as an extra MAIN-world `document_start` script (via `chrome.scripting`,
persisted across sessions); it defines `window.ethereum = window.pincer1.ethereum` *before any page script
runs*, so legacy dapps that only read `window.ethereum` see Pincer1. It is off by default because
(a) overwriting `window.ethereum` is exactly the behaviour that makes multi-wallet setups break — MetaMask,
Rabby and Pincer1 would fight over one global — and (b) a wallet that silently becomes the default signer
for every site is the opposite of what a consent-first device should do. If another wallet has already
locked `window.ethereum` (non-configurable), the takeover gives up silently. Reload open tabs after flipping
it either way.

### Bitcoin (`src/bitcoin/provider.ts`)

`window.pincer1.bitcoin` is kept small and matches what the common injected bitcoin wallets expose, so dapp
code written for Xverse / Leather / Unisat-style objects and for **sats-connect** works with light glue:

| Call | What happens |
| --- | --- |
| `requestAccounts()` | daemon `connect` → `[{ address, publicKey?, purpose: "payment", addressType: "p2wpkh" }]` |
| `getAccounts()` | same shape, only if the origin is already connected (no prompt) |
| `getNetwork()` | `{ bitcoin: { name: "Mainnet" }, stacks: { name: "Mainnet" } }` (sats-connect `wallet_getNetwork` shape) |
| `signPsbt(psbtBase64, { signInputs?, broadcast? })` | `{"t":"signTransaction","chain":"bitcoin","psbt","signInputs","finalize": broadcast}` → `{ psbt }`; with `broadcast: true` the daemon also finalizes and returns the raw tx, which the extension `POST`s to the Esplora-style API from the popup (default `https://mempool.space/api/tx`) → `{ psbt, txid }`. The extension never parses PSBTs. |
| `signMessage(address, message)` | `{"t":"signMessage","chain":"bitcoin","message","address"}` → BIP-322 signature (base64) |
| `request(method, params)` | sats-connect v2 / WBIP wallet-RPC envelope `{ jsonrpc, id, result }` / `{ …, error: { code, message } }` for `getAccounts`, `getAddresses`, `requestAccounts`, `signPsbt`, `signMessage`, `wallet_getNetwork`, `wallet_connect`, `wallet_disconnect` |

Discovery: a [WBIP004](https://wbips.netlify.app/wbips/WBIP004) entry `{ id: "pincer1", name: "Pincer1", icon, methods }`
is pushed onto `window.btc_providers`, which is what sats-connect's `getProviders()` and Xverse-style wallet
pickers read. sats-connect resolves the object by walking `window.<id>`, so `window.pincer1.request(...)`
forwards to the bitcoin provider. Error codes follow sats-connect's `RpcErrorCode`: **-32000** user
rejection (robot denied), **-32002** access denied (not connected / foreign address), **-32601** unsupported
method, **-32602** bad params, **-32603** internal.

Not implemented: Ordinals/taproot accounts (`purpose: "ordinals"`), `sendTransfer`, `signMultipleTransactions`,
testnet/signet, Stacks methods.

### Popup

- the three addresses with copy; connected sites; approved sites with *Remove*; the *Site wants to connect*
  card with *Approve* / *Deny*; *Forget pairing*
- **EVM chain & RPC**: the active chain (changing it switches every tab and fires `chainChanged`), and the
  RPC URL for the selected chain (empty = the built-in default); the *take over `window.ethereum`* toggle
- **Solana RPC** for mainnet sends; **Bitcoin API** (Esplora base URL) for broadcasts
- the pending request card (chain · kind · risk · summary · reasons · spoken lines · timer · origin) and the
  last 20 requests with their chain

## What is in the box

```
manifest.json              MV3: inpage.js (world MAIN) + content.js on <all_urls> at document_start, module
                           service worker, popup; permissions "storage" + "scripting" (+ host_permissions
                           <all_urls>, needed for the dynamically registered takeover script)
scripts/make-icons.mjs     pure-Node PNG encoder → public/icons/icon{16,48,128}.png + src/generated/icon.ts
scripts/build.mjs          five Vite builds (background ESM; content, inpage, takeover IIFE; popup page) → dist/
src/protocol.ts            §3 message types (with chain payloads) + the extension-internal frames
src/inpage.ts              Wallet Standard wallet (PincerWallet) + installWallets(): window.pincer1, EIP-6963,
                           btc_providers; honours the takeover flag
src/evm/chains.ts          the five chains, default RPCs, read-only method allow-list
src/evm/tx.ts              dapp tx → unsigned RLP (viem), fee/nonce/gas fill, signed-RLP tamper check
src/evm/provider.ts        Pincer1EthereumProvider (EIP-1193), announceEip6963, error mapping
src/bitcoin/provider.ts    Pincer1BitcoinProvider, WBIP004 entry
src/takeover.ts            the opt-in window.ethereum definer (only registered while the toggle is on)
src/lib/pageTransport.ts   page side of the postMessage framing (per-request id, same-window + same-origin check)
src/lib/contentBridge.ts   content side (chrome.runtime port, re-created if the worker dies)
src/lib/jsonRpc.ts         fetch JSON-RPC client + Esplora broadcaster
src/content.ts             wires contentBridge to chrome.runtime.connect
src/background/core.ts     the state machine: socket + backoff, pair/connect, per-chain requests, explain, log, badge
src/background.ts          wires core to chrome.storage / ports / badge / web3.js / fetch / chrome.scripting; MV3 keep-alive
src/popup/                 plain TS + HTML + CSS popup (Manrope / Geist Mono, lime #8ad414 on #0a0a0a)
test/                      vitest: framing, Solana wallet, EVM provider, Bitcoin provider, core vs mock daemon + fake RPC
```

Pinned dependencies: `viem 2.57.4` (RLP serialization/parsing, hex and address helpers — used in the
background and, for `isAddress`/`stringToHex`, in `inpage.js`), `@solana/web3.js 1.99.0`,
`@wallet-standard/core 1.1.2`, `@solana/wallet-standard-features 1.5.0`, `@solana/wallet-standard-chains 1.1.2`,
`bs58 6.0.0`; dev: `typescript 5.9.3`, `vite 7.3.7`, `vitest 5.0.3`, `@types/chrome 0.3.5`, `@types/node 24.10.1`.

Storage keys (`chrome.storage.local`): `daemonUrl`, `rpcUrl`, `token`, `pubkey`, `addresses`, `evmChainId`,
`evmRpcUrls`, `btcApiUrl`, `takeoverEthereum`, `log`.

Security choices worth knowing about:

- `inpage.js` only accepts replies whose `event.source === window` and `event.origin === location.origin`;
  `content.js` applies the same filter in the other direction. A per-request id matches replies to promises.
- Opaque origins (`null`, sandboxed frames) get no wallet at all.
- EVM fees, nonce and gas come from *your* RPC, and the signed transaction is re-parsed and compared with
  what was sent before it is broadcast. The daemon is untrusted by the protocol; the extension keeps it that way.
- `eth_sign` and `eth_signTypedData` v1 are refused; `wallet_addEthereumChain` never adds a dapp-supplied RPC.
- No telemetry, no remote code. The only network connections are the robot WebSocket, the RPC URLs you set
  and the bitcoin API you set (plus Google Fonts for the popup's two typefaces — a stylesheet, not script;
  drop the `<link>` from `src/popup/index.html` to go fully offline).
- Bundles are deliberately **not** minified so anyone can read what runs on every page.

## Limitations

- **MV3 worker lifetime.** Chrome stops an idle service worker after 30 s. While the socket is open the worker
  calls `chrome.runtime.getPlatformInfo()` every 20 s to stay alive (the documented workaround), and WebSocket
  traffic also resets the timer on Chrome ≥ 116. If the worker is still killed mid-request, the page gets an
  `INTERNAL` error and the robot's pending request simply expires.
- **Chrome only.** `"world": "MAIN"` content scripts need Chrome ≥ 111 (`minimum_chrome_version` is 116 for the
  WebSocket lifetime rule). Firefox MV3 would need a `web_accessible_resources` + script-tag injection path.
- **One pending request is shown** in the popup even if a dapp fires several; all of them are tracked and the
  badge shows the count.
- **Plain `ws://`.** The link to the robot is unencrypted on your LAN. Use `wss://` if your daemon terminates TLS.
- **No `window.solana` / `window.phantom` shim.** Sites that only look for Phantom's legacy object will not see Pincer1.
- **EVM:** one account, five chains, no EIP-2930/4844/7702 transaction types, no `eth_subscribe`
  (WebSocket) methods, no ENS; `eth_sendTransaction` returns when the RPC accepts the tx (no receipt wait).
  The chain is global, not per-origin: switching on one dapp switches all tabs (they are told).
- **Bitcoin:** payment address only, mainnet only; broadcasting needs a daemon that finalizes (`finalize: true`
  → `tx`), otherwise `signPsbt({ broadcast: true })` returns `SEND_FAILED` and the dapp can broadcast the
  returned PSBT itself.
- **`signAndSendTransaction` does not confirm.** It returns as soon as the RPC accepts the transaction.
- **No `solana:signIn`** (SIWS) and no `signAllTransactions` fan-out beyond signing inputs sequentially.

