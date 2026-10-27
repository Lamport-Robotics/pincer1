# pincerd

The wallet daemon that runs on Pincer1's Raspberry Pi 5. It is the untrusted middle: it parses and explains
every transaction, runs the scam filter, checks that the owner is in the room, makes the robot speak, and
then asks the consent controller for a signature. It never holds a key.

```
extension ──ws:7401──▶ pincerd ──USB──▶ consent controller ── SE050
                          │
                          └──ws:7402──▶ pincer_robot (lidar, face, voice, speech)
```

## Run on a laptop (no hardware)

```bash
pnpm install
cp .env.example .env            # CONTROLLER=sim, ROBOT_WS=mock
ROBOT_WS=mock pnpm dev
```

The sim controller uses a software ed25519 key and the same policy checks as the firmware. Simulate a touch:

```bash
curl -XPOST localhost:7401/dev/touch -d '{}'          # short touch
curl -XPOST localhost:7401/dev/touch -d '{"long":true}'
curl -XPOST localhost:7401/pair/new                    # pairing code for the extension
```

## Run on the robot

```
PINCER_MODE=prod CONTROLLER=/dev/ttyACM0 ROBOT_WS=ws://127.0.0.1:7402 pnpm dev
```

`prod` refuses to start against the software signer, a controller without its SE050 or trusted display,
or a bench image with compiled-in channel keys; refuses signatures when the owner is not verified in the
room; and locks the controller whenever presence is lost.

## Environment

See `.env.example`. Multi-chain additions: `EVM_RPC_<chainId>`, `ETHERSCAN_API_KEY`, `DAILY_LIMIT_ETH`,
`EVM_ROUTER_ALLOWLIST`, `BTC_NETWORK`, `BTC_ESPLORA_URL`, `BTC_MAX_FEE_RATE`, `BTC_FEE_TARGET_BLOCKS`,
`DAILY_LIMIT_BTC`, `ONEINCH_API_KEY`, `ZEROX_API_KEY`, `THORNODE_URL`, `THORNODE_URL_2`, `SWAP_MAX_SLIPPAGE_BPS`.

Pinned: `viem 2.57.4`, `bitcoinjs-lib 6.1.7`, `@noble/curves 1.9.7` (sim signing and signature verification only; the daemon never signs).

## What happens on `signTransaction`

1. `tx/parse.ts` turns the message into an owner-centric view: SOL out, tokens out, programs, dangers
   (setAuthority, approve, closeAccount, assign, lookup tables, durable nonce). v0 lookup tables are resolved
   through the RPC.
2. `risk/intel.ts` fetches what the chain knows about every counterparty: age, activity, executable, mint
   authority. Cached, best-effort, never blocks for more than 4 s.
3. `tx/explain.ts` writes the sentences the robot says.
4. `risk/filter.ts` decides `low | medium | high | block`. `block` is refused without asking.
5. Presence: the robot must report `person && owner && !duress && !moved` within the last 6 s.
6. The robot turns to the owner and speaks; `claims` are sent to the controller with the bytes.
7. The controller re-parses the bytes itself and signs only if the claims hold and a touch arrives.

The sim controller reproduces step 7 in software, so the tests exercise the full path.

## Tests

```bash
pnpm test        # parse, explain, scam filter, full pipeline with sim + mock robot, extension ws contract, agent rules,
                 # EVM parse/explain/risk/EIP-712/pipeline, Bitcoin PSBT build/risk/finalise, swaps with fake routers, sim chain policy, mode gating
pnpm typecheck
```

## Overnight agent

Disabled by default (`AGENT_ENABLED=false`). When enabled, the owner grants a one-night allowance on the
controller with a long touch (`Agent.arm`); trades that fit the allowance (Jupiter only, per-trade and
cumulative caps in SOL, no dangerous instructions, no token outflows at the message level) are signed
without a touch. `AGENT_ENABLED` accepts `true`/`1` and `false`/`0` only. Everything else becomes a morning
proposal that goes through the normal touch-gated path. Signals come from `SIGNALS_URL` (`/signals`,
`/prices?mints=`) or a mock provider.

## Multi-chain (Solana, EVM, Bitcoin)

One seed in the SE050, two key families: ed25519 for Solana, secp256k1 for EVM chains and Bitcoin
(PROTOCOL §1 "Multi-chain signing"). `hello.addresses` lists all three; `GET /status` echoes them with the
wallet `mode`. Every chain goes through the same pipeline in `src/sign.ts` — parse → intel → explain → scam
filter → presence → speak → controller → assemble — via a chain adapter (`src/chains/types.ts`):

| chain | adapter | parses | controller kind | assembles |
|---|---|---|---|---|
| Solana | `src/chains/solana/` (wraps `tx/`, `risk/`) | legacy + v0 messages | `tx`, `msg` | adds the ed25519 signature |
| EVM | `src/chains/evm/` (viem) | unsigned legacy / EIP-1559 RLP; ERC-20 `transfer` / `approve` / `transferFrom` / `increaseAllowance`, `setApprovalForAll`, ERC-721/1155 `safeTransferFrom`, EIP-2612 / DAI `permit`, Permit2 | `evm_tx`, `evm_msg` | 65-byte r‖s‖v → signed RLP; the recovered signer must equal the owner |
| Bitcoin | `src/chains/bitcoin/` (bitcoinjs-lib) | BIP-174 PSBT, P2WPKH | `btc_psbt` | verifies the controller's partial signatures, finalises, extracts |

**EVM.** Chains: Ethereum 1, Base 8453, Arbitrum 42161, BNB 56, Polygon 137 (any id with `EVM_RPC_<chainId>`).
Intel per RPC: `eth_getCode`, balance, nonce, ERC-20 symbol/decimals by `eth_call`, contract age by probing
`eth_getCode` at older blocks (two bucket probes, a short bisection, one timestamp; bounded) or from an
Etherscan-compatible API when `ETHERSCAN_API_KEY` is set (also gives source verification). Rules (`evm/risk.ts`):
unlimited approve → **block** (even to a known router, unless `settings.unlimitedApproveToRouters` is on →
medium); any approve to a plain wallet → block; `setApprovalForAll(true)` → block; chain id ≠ the dapp's
request → block; value > 90 % of balance → block; call to an address with no code and no history → block;
unknown contract with no age and no verified source → high; destination contract < 3 days old → high; fee
> 10 % of value → medium; above the daily limit → medium. Messages: EIP-191 bytes are refused when they parse
as a transaction; EIP-712 typed data is hashed with viem, permits (EIP-2612, DAI, Permit2 single/batch/
transfer) are explained like approvals, unlimited or > 30-day deadlines → high, spender is a plain wallet →
block. The controller gets the digest *and* the canonical JSON and re-hashes it.

**Bitcoin.** UTXOs, fee estimates and broadcast via an Esplora-compatible API (`BTC_ESPLORA_URL`, 4 s
timeout). Fee rate from `/fee-estimates` for `BTC_FEE_TARGET_BLOCKS`, clamped to `BTC_MAX_FEE_RATE`; a lying
oracle cannot burn coins. Coin selection largest-first, change back to the owner's own address when ≥ 546
sats (otherwise folded into the fee), RBF on. Rules (`bitcoin/explain.ts`): fee > 5 % of amount → high
(> 2 % medium); any output that is neither the destination nor the owner's change → **block**; sighash ≠ ALL
→ block; inputs without a value → block; dust-sized inputs (< 1,000 sats) → medium; destination used ≥ 20
times → medium; > 90 % of balance → block. Spoken: "Sends 0.0015 BTC to bc1qar0s…5mdq, fee 2,100 sats (1.4%)."

**Controller twin.** `controller/sim.ts` enforces PROTOCOL §1's EVM checks (chain id, `to`, value, worst-case
fee, token amounts, declared dangers, daily limit in wei) and Bitcoin checks (network, non-change outputs,
destination, fee, declared dangers incl. `high_fee`, input count, daily limit in sats) with the same parsers,
signing with `@noble/curves` secp256k1 (sim only). `hello.policy` carries `daily_limit_wei` / `daily_limit_sats`;
`setChainPolicy` changes them with a long touch.

### Swaps (online mode only)

`POST /swap/quote {from:{chain,asset,chainId?}, to:{chain,asset,chainId?}, amount}` → the robot speaks the
quote ("Swapping 1,000 USDC on Ethereum for about 0.5 WETH through 1inch. You'll get at least 0.495 WETH, that's
1% slippage. Say execute within ten minutes."). `POST /swap/execute {quoteId}` builds the transaction through
the router, re-verifies it, then runs the normal presence + explain + touch pipeline and broadcasts.

Routes: Solana via Jupiter (lite API, v6 program); EVM via 1inch v6 (`ONEINCH_API_KEY`) with 0x as fallback
(`ZEROX_API_KEY`); cross-chain Bitcoin ⇄ EVM native coins via THORChain (`THORNODE_URL` and `THORNODE_URL_2`:
an inbound address is pinned only when both nodes agree on it).

Safety rules, every swap, no exceptions (`src/swap/verify.ts`, `src/swap/index.ts`):

1. **Router allowlist.** Jupiter v6 `JUP6Lk…VTaV4`; 1inch v6 `0x1111111254…842A65`; 0x Exchange Proxy
   `0xDef1C0ded9…b25EfF`; THORChain router and inbound vaults fetched live and pinned for 10 minutes. The built
   transaction's `to` / program must equal the allowlisted router or it is refused before the controller sees it.
2. **Minimum output from the bytes, not the JSON.** Jupiter route instruction tail (`quoted_out_amount`,
   `slippage_bps`), 1inch `swap` / `unoswap*` / `ethUnoswap*` `minReturn`, 0x `transformERC20`
   `minOutputTokenAmount`, THORChain memo limit — decoded and required to be ≥ quote × (1 − slippage).
   Unknown calldata is refused, exact-out routes are refused.
3. **Slippage capped at 1 %** (`SWAP_MAX_SLIPPAGE_BPS`, `settings.swapMaxSlippageBps` may lower it); a higher
   request is clamped, a route carrying more slippage than the cap is refused.
4. **Approvals are exact-amount, to the router only, never unlimited.** Issued only when the current allowance
   is short; the built approval is re-parsed before signing.
5. **Amounts and receiver.** The router may not spend more than quoted, may not send native value on a token
   swap, and the output receiver (when encoded) must be the owner.
6. **One execution per quote**, quotes expire after ten minutes, and `settings.mode === "offline"` refuses
   both endpoints with `offline_mode`.

### Mode and brain

`settings.mode` (`online` default | `offline`): offline refuses swaps and cloud brains. `settings.brain`
(`lamport` default | `claude` | `openai` | `deepseek`): choosing a cloud brain while offline fails with
`offline_mode` and a sentence saying what to change. Both are pushed to the robot with `robot.settings`.

## Optional features (off by default, `GET/POST /settings`)

| setting | effect |
|---|---|
| `roomLock`, `roomMatchMin` | refuse when the robot's `room_match` is below the threshold |
| `twoPersonSol` | transfers ≥ this many SOL need `persons ≥ 2` and two enrolled `owners` |
| `offlinePayments` | enables `/offline/sign`, `/offline/pending`, `/offline/broadcast`, `/offline/flush`, `/offline/nonce/create` |
| `inheritance` | intent only; arming is `POST /inheritance/arm` (long touch on the controller), status `GET /inheritance`, `clear`, `release` |
| `airgap` | `off` / `strict` / `night`, pushed to the robot |
| alerts | `GET/POST /alerts`, `DELETE /alerts/:id`; wake in person or phone via `NTFY_TOPIC` / Telegram / webhook |
| `mode`, `brain` | `offline` refuses swaps and cloud brains; `brain` ∈ lamport / claude / openai / deepseek |
| `unlimitedApproveToRouters` | tolerate unlimited ERC-20 approvals when the spender is an allowlisted router (default off: exact amounts only) |
| `swapMaxSlippageBps` | lower the 1 % swap slippage cap |

## Files

| path | role |
|---|---|
| `src/chains/` | `types.ts` adapter contract; `solana/`, `evm/` (rpc, abi, parse, intel, explain, risk, message), `bitcoin/` (esplora, psbt, explain) |
| `src/swap/` | routers allowlist + THORChain pins, independent verification, providers (Jupiter, 1inch, 0x, THORChain), owner-facing service |
| `src/tx/` | Solana parser, explainer, known programs and mints |
| `src/risk/` | address intel (RPC) and the scam filter |
| `src/controller/` | `types.ts` contract, `serial.ts` real controller, `sim.ts` software twin |
| `src/presence/` | robot client (ws) and a mock |
| `src/sign.ts` | the pipeline |
| `src/api/server.ts` | extension WebSocket + local HTTP |
| `src/agent/` | rules, signal providers, Jupiter, morning report |
| `src/settings.ts` | owner toggles, persisted |
| `src/offline/` | durable-nonce signing, QR, deferred broadcast |
| `src/inheritance/` | builds the heir tx, relays controller warnings, broadcasts on expiry |
| `src/alerts/` | portfolio rules, escalation (robot wake → phone) |
