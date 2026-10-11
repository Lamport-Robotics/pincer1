# Pincer1 internal protocol

Three processes talk to each other. Every link is newline-delimited JSON (one object per line, UTF-8).

```
 browser ──(Wallet Standard)──▶ extension ──ws://robot:7401──▶ pincerd (daemon, Pi 5, TypeScript)
                                                                 │  ▲
                                            ws://127.0.0.1:7402  │  │  USB CDC serial (/dev/ttyACM0)
                                                                 ▼  │
                                     pincer_robot (Python: lidar, face, voice, speech, motion)
                                                                    │
                                                   consent controller (RP2350 firmware) ── I2C (SCP03) ── SE050
```

Trust boundaries:

- **Consent controller + SE050** are the only trusted parts. A signature exists only if the controller
  produced it, and the controller produces one only after a physical touch *and* after its own parse of
  the transaction agrees with what the daemon claimed about it.
- **pincerd** and **pincer_robot** run on the Pi and are treated as compromisable. Their presence checks
  (lidar, face, voice) stop a *remote* attacker from even reaching the touch step and protect the owner from
  social engineering, but they are not what makes the key safe.
- **extension** is untrusted input.

## 1. Daemon ⇄ consent controller (USB CDC)

Host → controller requests carry an `id`; the controller answers with the same `id`. Unsolicited controller
messages (`event`) carry no `id`.

### hello

```json
→ {"t":"hello"}
← {"t":"hello","fw":"0.4.0","se050":true,"initialized":true,"pubkey":"<base58>",
   "addresses":{"solana":"<base58>","evm":"0x…","bitcoin":"bc1q…"},"locked":false,
   "policy":{"daily_limit_lamports":"2000000000","spent_today_lamports":"0","day":20395,
             "daily_limit_wei":"0","spent_today_wei":"0","daily_limit_sats":"0","spent_today_sats":"0"},
   "allowance":null,"pin_set":true,"duress_pin_set":true,"pin_failures":0,"inheritance":{…},
   "display":true,
   "routers":{"builtin":[{"chain":"solana","address":"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"},
                         {"chain":"evm","address":"0x111111125421cA6dc452d289314280a0f8842A65"},
                         {"chain":"evm","address":"0xDef1C0ded9bec7F1a1670819833240f027b25EfF"}],
              "owner":[]}}
```

`addresses` are the active slot's (the decoy has its own three) and `null` before `init`; `pubkey` stays for
compatibility and equals `addresses.solana`. All amounts are decimal strings; wei values are 256-bit.

`se050:false` means the controller runs with its software signer (dev boards without the chip), or that
the SE050 could not be opened (no SCP03 keys, handshake failed). The daemon refuses to run in production
mode against a software signer. `pin_set` says whether the init flow finished with `set_pin`; `duress_pin_set`
whether a decoy rhythm exists (whether duress is *active* is never reported);
`pin_failures` is the controller's mirror of the SE050's wrong-PIN counter (5 wipes the device). An extra
`"dev_keys":true` appears when the SE050 channel keys were compiled into the firmware instead of read from
OTP (bench build); production mode must refuse that too. `display:true` says the controller drives its
trusted display (`display:false` = a `PINCER_NO_DISPLAY` bench image; production mode must refuse that as
well). `routers` is the controller's swap-router allowlist: the built-in set and the owner-added entries
(see `policy`).

### The trusted display

The controller renders every request on its own 128×64 OLED **from its own parse**, never from any text the
daemon sends (`summary`, `claims`, ids). The daemon's speech is a convenience for the owner; the display is
the consent channel. For `sign` and `inheritance_set` the request is first in `need:"display"` (internal),
becomes `pending` with `need:"touch"` only once the first page is on the screen, and a touch counts as
consent only after the **last** page has been shown: earlier touches turn pages. Holding the pad 3 s cancels
(`denied:user`). Every rendered page is announced:

```json
{"t":"event","ev":"display","page":1,"pages":3}
```

so the daemon can keep speech in sync with the page the owner is reading. The `pending` reply carries
`parsed` (the same reading the display shows) for the spoken explanation.

### Pending requests and what they need

Every request that needs the owner answers first with `pending` and a `need`:

| `need` | the owner does | used by |
|---|---|---|
| `touch` | one tap on the head, on the **last page** of the display (earlier taps turn pages; hold 3 s = cancel) | `sign`, `unlock` before a PIN exists, `inheritance_set` replacing an existing arrangement, the seed-word pages after `init` |
| `long_touch` | hold 3 s | `init`, `elevate`, `allowance`, `inheritance_set` (arming), `inheritance_clear` |
| `pin` | tap the PIN rhythm (4–8 taps, short/long gaps; a 1.5 s pause ends it) | `set_pin`, `unlock`, `policy` |

`pending` may carry `"confirm":true` (`set_pin` asks for the rhythm a second time). While a request needs
the PIN, taps are rhythm input and do not approve anything; the display shows one dot per tap and the
operation being authorised.

### init (first boot only, requires a long touch of 3 s, then the word pages)

```json
→ {"t":"init","id":"i1"}
← {"t":"pending","id":"i1","need":"long_touch"}
← {"t":"pending","id":"i1","need":"touch"}            // keys created; the words are on the display
← {"t":"event","ev":"display","page":1,"pages":28}    // … one per page the owner taps through
← {"t":"init","id":"i1","pubkey":"<base58>","addresses":{"solana":"<base58>","evm":"0x…","bitcoin":"bc1q…"},
   "decoy_addresses":{"solana":"<base58>","evm":"0x…","bitcoin":"bc1q…"}}
   | {"t":"denied","id":"i1","reason":"user"|"timeout"|"locked"|"policy"}
```

`decoy_addresses` are the second (decoy) wallet's public keys: they appear here, once, so the owner can fund
the decoy, and never in `hello` (which always shows the *active* slot as `addresses`). They can be read again
later with the PIN-gated `decoy_addresses` request below.

The 24 words are generated from the controller's hardware RNG; the ed25519 key is derived with SLIP-0010
(`m/44'/501'/0'/0'`) and the two secp256k1 keys with BIP32 (`m/44'/60'/0'/0/0`, `m/84'/0'/0'/0/0`), all
three are imported into the SE050 as non-exportable keys whose policy allows signing only in a
PIN-authenticated session, and the seed bytes are zeroed from RAM. **The words never cross this link.** They
are shown on the controller's display only: the real set four words per page (6 pages), a check page, the
real set again, then the decoy set the same way, 28 pages in all, advanced by short touches. Holding the pad
3 s, ten minutes without a touch, `lock` or `duress` abort the init: the imported keys are destroyed and the
request is denied. Only the touch on the last page makes the device `initialized` and produces the `init`
reply, which carries `pubkey` (Solana) and `addresses` and nothing else. The daemon's setup UI tells the
owner to look at the robot and write; it never sees the words.

```json
→ {"t":"reveal_seed","id":"x1"}   ← {"t":"denied","id":"x1","reason":"not_supported"}
```

`reveal_seed` is a deliberate non-feature: there is no command that returns the mnemonic, the decoy
mnemonic, or any key material, and the controller answers `not_supported` so the refusal is explicit.

### set_pin (init flow, step 2, once) and set_duress_pin (step 3, optional, replaceable)

```json
→ {"t":"set_pin","id":"n1"}
← {"t":"pending","id":"n1","need":"pin"}                   // tap the rhythm
← {"t":"pending","id":"n1","need":"pin","confirm":true}    // tap it again
← {"t":"set_pin","id":"n1","ok":true}
   | {"t":"denied","id":"n1","reason":"pin_mismatch"|"timeout"|"policy"|"not_initialized"}
```

Creates the PIN object in the SE050 (UserID, 5 attempts). Until it exists every `sign` and `policy` is
denied with `pin_not_set`; `hello` shows `pin_set:false`. The PIN cannot be changed afterwards: a new
PIN means a new wallet (`init` after a wipe). The rhythm itself never crosses this link.

```json
→ {"t":"set_duress_pin","id":"d1"}
← {"t":"pending","id":"d1","need":"pin"}                   // tap your REAL rhythm (skipped right after set_pin)
← {"t":"pending","id":"d1","need":"pin"}                   // tap the second rhythm
← {"t":"pending","id":"d1","need":"pin","confirm":true}    // tap it again
← {"t":"set_duress_pin","id":"d1","ok":true}
   | {"t":"denied","id":"d1","reason":"pin_mismatch"|"wrong_pin"|"timeout"|"policy"|"pin_not_set"|"locked"|"busy"}
```

A **second rhythm** that opens the decoy wallet (see `unlock`). PIN-gated: the owner first taps the real
rhythm, then the new one twice; sent directly after `set_pin` (init flow step 3) the real rhythm is still
known to the controller and is not asked again. The new rhythm must differ from the real one and its two
entries must match, otherwise `pin_mismatch` (nothing counted). The controller stores it as a second UserID
object in the SE050 whose session may sign only with the decoy keys (the software signer keeps a salted
hash). It can be replaced any time under the real PIN; it is refused while the decoy slot is active. No
reply, event or `hello` field ever says whether one exists.

### sign

```json
→ {"t":"sign","id":"s7","kind":"tx","bytes":"<base64 serialized message>",
   "claims":{"lamports_out":"500000000","token_out":[{"mint":"<base58>","amount":"1000000","dest":"<base58 token account>"}],
             "dest":"<base58>","programs":["11111111111111111111111111111111"]},
   "ttl_ms":60000}
← {"t":"event","ev":"display","page":1,"pages":1}
← {"t":"pending","id":"s7","need":"touch","parsed":{"lamports_out":"500000000","dest":"<base58>",
                                                    "token_out":[{"mint":"…","dest":"…","amount":"1000000"}],"dangerous":[…]}}
← {"t":"signed","id":"s7","sig":"<base64 64 bytes>"}
   | {"t":"denied","id":"s7","reason":"timeout"|"claims_mismatch"|"over_daily_limit"|"locked"|"policy"|"user"}
```

`kind` is `tx` (a serialized Solana *message*, legacy or v0; the controller parses it) or `msg`
(an arbitrary byte string for `signMessage`; the controller refuses anything that parses as a transaction
message, so a message can never be a disguised transaction).

Controller-side checks before touch is even requested:

1. The message's fee payer / first signer equals the controller's public key.
2. `parsed.lamports_out` (sum of System `transfer` / `transferWithSeed` / `createAccount` lamports leaving
   the owner) ≤ `claims.lamports_out`, and when it is > 0 the parsed destination (the first transfer's
   recipient) **equals** `claims.dest`. Every SPL `transfer` / `transferChecked` from the owner's token
   accounts must be covered by a `claims.token_out` entry: amount ≤ the claimed amount for that mint and
   the recipient token account equal to the entry's `dest` (`transferChecked` names the mint; a plain
   `transfer` is bounded by the claimed total and still needs its recipient named). A recipient the daemon
   did not name is `claims_mismatch`.
3. Every program id in the message is in `claims.programs`.
4. If the message contains `SetAuthority`, `Approve`, `CloseAccount` or `Assign` affecting an owner-owned
   account, `claims` must list it under `dangerous:[…]`; otherwise `denied:policy`. A program that is
   neither a core program (System, Token, Token-2022, Associated Token, Compute Budget, Memo) nor in the
   controller's router allowlist is `unknown_program`: it must be declared the same way and is shown as
   DANGER on the display.
5. `spent_today + lamports_out ≤ daily_limit`, unless the request was preceded by `elevate`.

The claims mechanism is what stops a compromised daemon from describing "0.5 SOL" out loud while the
bytes say 50 SOL: the controller computes the number itself, and the display shows the controller's number.


### Multi-chain signing (EVM and Bitcoin)

One seed, three key families inside the SE050: ed25519 for Solana, secp256k1 for EVM chains and for
Bitcoin. `hello` lists every address:

```json
"addresses":{"solana":"<base58>","evm":"0x…","bitcoin":"bc1q…"}
```

Derivation: Solana `m/44'/501'/0'/0'` (SLIP-0010), EVM `m/44'/60'/0'/0/0` (BIP32), Bitcoin native segwit
`m/84'/0'/0'/0/0` (BIP32, P2WPKH, mainnet only). The same 24 words restore all three in any standard wallet.
The controller holds exactly one key per chain; it signs for no other address or account.

`sign` gains two transaction kinds and two message kinds. The controller parses each on-device and compares
with `claims`, exactly as for Solana; the daemon's chain adapters produce the same numbers for the spoken
explanation. The reply for the transaction kinds is `pending` with the controller's own `parsed` view, then
`signed` after the touch.

**EVM** — `kind:"evm_tx"`, `bytes` = the unsigned signing payload of a legacy (type 0, EIP-155: nine RLP
fields with the chain id; pre-EIP-155 six-field transactions are refused) or EIP-1559 (type 2:
`0x02 ‖ RLP`) transaction. Type 1 and anything else is refused. Parsed on-device: chainId, nonce, to,
value, data, gas limit, max fee per gas (gas price for legacy), access list (ignored). Recognised calldata:
ERC-20 `transfer(address,uint256)` `0xa9059cbb`, `approve(address,uint256)` `0x095ea7b3`,
`transferFrom(address,address,uint256)` `0x23b872dd`, `setApprovalForAll(address,bool)` `0xa22cb465`,
`increaseAllowance(address,uint256)` `0x39509351`, each with its exact ABI length. Any other calldata is a
contract call: fine when `to` is in the **controller's own** router allowlist (built in: 1inch v6
`0x111111125421cA6dc452d289314280a0f8842A65`, 0x Exchange Proxy `0xDef1C0ded9bec7F1a1670819833240f027b25EfF`;
plus owner-added entries, see `policy`), otherwise `unknown_contract`. Contract creation (no `to`) is always
`unknown_contract`. A `claims.routers` array is accepted and ignored: the host cannot whitelist anything.

```json
"claims":{"chain_id":1,"to":"0x…","value_wei":"500000000000000000",
          "token_out":[{"token":"0x…","to":"0x…","amount":"1000000"}],
          "max_fee_wei":"2100000000000000",
          "dangerous":["approve"|"approve_unlimited"|"set_approval_for_all"|"unknown_contract"]}
```

Controller checks: (1) `chain_id` equals claimed and `to` equals claimed (both absent for a creation);
(2) `value` ≤ `value_wei`; (3) `gas_limit × max_fee_per_gas` ≤ `max_fee_wei` (an overflowing product never
passes); (4) every ERC-20 amount leaving the owner — `transfer`, and `transferFrom` whose `from` is the owner
— ≤ the claimed `token_out` amount for that token contract **and** its recipient equals the entry's `to`
(an entry without `to` covers nothing); (5) `approve` of a finite amount or
`increaseAllowance` → `approve`, `approve` of 2^256−1 → `approve_unlimited` (the two are exclusive, the
daemon declares the one that applies), `setApprovalForAll(true)` → `set_approval_for_all`, calls into a `to`
that is neither a plain transfer, a recognised ERC-20 call nor an allowlisted router, and creations →
`unknown_contract`; each must be declared under `dangerous` or the request is `denied:policy`, and each is a
boxed DANGER line on the display. (6) The daily
limit in wei: `spent_today_wei + value ≤ daily_limit_wei` (`policy.daily_limit_wei`), unless an EVM `elevate`
preceded the request. ERC-20 amounts do not count against the wei limit (the controller cannot price them);
`token_out` is the bound. `pending.parsed` =
`{"chain_id","nonce","to","value_wei","max_fee_wei","call","token_out"|"spender"+"allowance","dangerous"}`.
The signature is 65 bytes `r‖s‖v` (base64 in `sig`), low-s; `v` = 27/28 for legacy (the daemon turns it
into `chainId·2+35+recid`) and 0/1 for typed transactions; the daemon assembles the signed RLP.

**Bitcoin** — `kind:"btc_psbt"`, `bytes` = a BIP-174 PSBT (v0; PSBTv2 refused; ≤ 8 inputs, ≤ 8 outputs,
≤ 4096 bytes). Parsed on-device: every input the owner's key spends (`bip32_derivation` naming the
controller's key at `84'/0'/0'/0/0` **and** a `witness_utxo` paying `0014‖hash160(key)`), every output,
change outputs recognised by key path (`84'/0'/0'/1/i`, re-derived from the account xpub the controller keeps,
or an output to the controller's own address), fee = inputs − outputs. A PSBT with an input the owner's key
does not control is refused (`denied:policy`): the controller does not co-sign into transactions it cannot
account for.

```json
"claims":{"network":"mainnet","sats_out":"150000","dest":"bc1q…","fee_sats":"2100","inputs":2,
          "dangerous":["high_fee"|"unknown_change"|"sighash_not_all"]}
```

Controller checks: (1) `network` is `mainnet` (the only network the controller has keys for) and, when
`inputs` is given, it equals the number of owner inputs; (2) the sum of non-change outputs ≤ `sats_out`;
(3) fee ≤ `fee_sats`; (4) a PSBT that spends anything must carry `dest`, the first non-change output must be
exactly that scriptPubKey (`claims_mismatch` otherwise, declared or not), and every further output that is
neither `dest` nor recognised change must be declared `unknown_change`; (5) every owner input's sighash must be `ALL` unless `sighash_not_all` is declared;
`SIGHASH_NONE` (and `SINGLE` with no matching output) is never signed, declared or not; a fee ≥ 10 % of what
leaves (or any fee when nothing leaves, i.e. a consolidation) must be declared `high_fee`. (6) The daily limit
in sats: `spent_today_sats + sats_out ≤ daily_limit_sats`, unless a Bitcoin `elevate` preceded the request.
`pending.parsed` = `{"sats_out","fee_sats","change_sats","dest","inputs","outputs","dangerous"}`. The
controller signs each owner input (BIP-143 sighash, DER low-s) and returns
`{"t":"signed","id":…,"psbt":"<base64>"}`, the PSBT with `PSBT_IN_PARTIAL_SIG` entries added; the daemon
finalises and broadcasts.

**Messages.** `kind:"evm_msg"` with `bytes` = EIP-191 `personal_sign` (prefix and hashing on-device;
65-byte `r‖s‖v`, `v` = 27/28), or with `"typed_data":{…}` (the EIP-712 JSON: `types`, `primaryType`,
`domain`, `message`) and `"digest":"0x…"` instead of `bytes`: the controller re-hashes domain and message
and signs only if its digest equals the claimed one (`denied:claims_mismatch` otherwise). Only structs of
primitive types (`address`, `bool`, `uintN`, `intN`, `bytesN`, `bytes`, `string`) are encoded; nested structs
and arrays are refused with `denied:unsupported_typed_data` — the controller never signs a typed-data digest
it could not verify. `kind:"btc_msg"` with `bytes` = the message: BIP-322 simple for the P2WPKH address,
`sig` = base64 of the witness stack (`02 <DER‖01> <pubkey>`). Messages need a touch, never an allowance, and
do not count against any limit. A message of any kind whose bytes parse as a Solana message, an EVM
transaction or a PSBT is refused (`denied:policy`), so a message can never be a disguised transaction.

`inheritance_set` remains Solana-only (a durable-nonce Solana message). `allowance` remains Solana-only.

**Swaps** (online mode only). The daemon builds swap transactions only through allowlisted routers:
Solana Jupiter v6; EVM 1inch v6 and 0x Exchange Proxy; cross-chain THORChain. The minimum output is embedded
in the transaction by the router; the daemon verifies the router address and that the quote's minimum output
matches the calldata before asking the controller. The controller keeps **its own** allowlist (built in:
Jupiter v6 `JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4`, 1inch v6, 0x; THORChain routers are per chain and
must be added by the owner) and treats any other program / contract as `unknown_program` /
`unknown_contract`: the daemon must declare it, and the owner sees DANGER on the display. The daemon's
`claims.routers` is informational only.

### elevate (temporarily raise the daily limit for one request)

```json
→ {"t":"elevate","id":"e1","limit_lamports":"20000000000"}      // or "limit_wei":"…" | "limit_sats":"…" (exactly one)
← {"t":"pending","id":"e1","need":"long_touch"}
← {"t":"elevated","id":"e1","until_ms":120000}
```

An elevate is bound to the chain of the limit it names: it raises that chain's limit for one touched request
on that chain within the window and is consumed by it; requests on other chains neither use nor consume it.

### allowance (pre-authorised budget for the overnight agent)

```json
→ {"t":"allowance","id":"a1","lamports":"1000000000","programs":["JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"],
   "until_unix":1791650000,"max_per_tx_lamports":"200000000"}
← {"t":"pending","id":"a1","need":"long_touch"}
← {"t":"allowance","id":"a1","ok":true}
```

While an allowance is active, a `sign` request whose parsed `programs` ⊆ allowance programs, whose
`lamports_out` ≤ `max_per_tx_lamports`, whose cumulative spend ≤ `lamports`, and which carries no
`dangerous` instruction is signed **without a touch**. Everything else still needs a touch. The owner
grants the allowance with a long touch before going to sleep; it expires at `until_unix` or on `lock`,
`duress`, or `{"t":"allowance","id":"a2","revoke":true}`. `hello` reports the active allowance (or null).


### inheritance (dead-man's switch, timer lives on the controller)

The controller has no clock, so it keeps a **powered-uptime counter** (seconds, from its own 5 ms tick)
persisted to flash at least hourly, and treats a physical touch as the only proof of life:
`days_since_touch = (powered_seconds − last_touch_powered_seconds) / 86400`. Host time plays no part. A
compromised daemon can neither fake touches nor fast-forward the counter; the only way to make the timer
expire is to keep the robot powered and untouched for the whole period. (Conversely, time spent powered
off does not count, and up to one hour of uptime can be lost per power cut.)

```json
→ {"t":"inheritance_set","id":"h1","bytes":"<base64 serialized message (durable-nonce tx)>",
   "claims":{…same as sign…},"inactivity_days":90,"warn_days":[14,7,1],"heir":"<base58>"}
← {"t":"pending","id":"h1","need":"long_touch","parsed":{…same as sign…}}   // after event:display, like sign
← {"t":"inheritance_set","id":"h1","ok":true,"sig":"<base64>"}          // signed, stored, tx NOT returned
   | {"t":"denied","id":"h1","reason":"policy"|"claims_mismatch"|"not_initialized"|"pin_not_set"|"locked"|"busy"|"timeout"}
→ {"t":"inheritance_status"}
← {"t":"inheritance_status","armed":true,"inactivity_days":90,"days_since_touch":12.4,
   "powered_days":103.0,"heir":"<base58>","warn_days":[14,7,1],"warned":[]}
→ {"t":"inheritance_release","id":"h2"}
← {"t":"inheritance_release","id":"h2","tx":"<base64 signed wire transaction>"}   // only after the timer expired
   | {"t":"denied","id":"h2","reason":"not_expired"|"not_armed"}
→ {"t":"inheritance_clear","id":"h3"}   ← pending long_touch ← {"t":"inheritance_clear","id":"h3","ok":true}
   | {"t":"denied","id":"h3","reason":"not_armed"|"locked"|"policy"|"not_initialized"|"pin_not_set"|"busy"|"timeout"}
```

Rules:

- `inheritance_set` goes through the same controller-side checks as `sign` (fee payer is the owner, parsed
  lamports / tokens / programs / dangerous instructions against `claims`), and in addition the message
  **must be a durable-nonce transaction**: its first instruction is System `advanceNonce` (index 4) and
  the nonce account is not the owner key. The nonce account should be dedicated to inheritance and never
  advanced otherwise, or the stored transaction dies silently. The message must fit one SE05x APDU
  (≤ 860 bytes) and have at most 8 required signers. `inactivity_days` ≥ 1; up to 4 `warn_days`, each
  ≥ 1 and below `inactivity_days`. Refused in duress (the decoy never signs an inheritance) and while
  locked.
- Arming needs a **long touch**. Replacing an existing arrangement (the daemon prompts the owner to re-sign
  when balances drift, since the amount is fixed at signing time) needs a plain **touch**, like a `sign`.
  The approving touch starts the clock; `warned` is reset.
- The controller signs once with the real key, stores message + signature in its own flash region, and
  answers with the signature only. `inheritance_release` returns the bytes only when
  `days_since_touch ≥ inactivity_days`; it is a read, works any number of times, and works **while locked,
  in duress, uninitialised and after the PIN wipe**: `lock`, `duress` and the five-failure wipe leave the
  inheritance region untouched, so a thief cannot cancel the arrangement by triggering them. Only
  `inheritance_clear` with a long touch removes it, and it needs owner authority: initialised, PIN set,
  unlocked (so the PIN rhythm was verified), not in duress, **and the controller's real key must be the key
  that signed the arrangement** (stored with it). The same key binding applies to replacing it with
  `inheritance_set`. A wiped controller, or one a thief re-initialised with fresh keys, can therefore
  neither clear nor replace the arrangement; it can only wait for the release (or keep touching the pad).
  The legitimate owner who wipes and re-initialises is in the same position: the old arrangement stays
  (it spends the old account) until it is released; move the funds with the 24 words if that is unwanted.
- `tx` is a Solana wire transaction: `shortvec(num_required_signatures) ‖ sig ‖ 64 zero bytes per further
  signer ‖ message`. With a single signer (owner = fee payer = nonce authority) it is complete and can be
  broadcast as is; with more signers the heir adds theirs.
- **Any** debounced touch (tap, long touch, every rhythm tap, approved or not, even a thief's) resets
  `days_since_touch` and re-arms the warnings.
- Events as the timer runs: `{"t":"event","ev":"inheritance_warning","days_left":7}` once per `warn_days`
  threshold (`days_left` is the threshold value) when the remaining time drops to or below it; which
  thresholds fired is persisted (`warned`), so a reboot does not repeat them. A touch clears `warned`.
- `hello` carries `"inheritance":{"armed":true,"inactivity_days":90,"days_since_touch":12.4}`
  (`inactivity_days` is 0 when not armed). `days_since_touch` / `powered_days` are printed with one decimal.

### lock / unlock

```json
→ {"t":"lock"}         ← {"t":"locked"}
→ {"t":"unlock","id":"u1"}  ← {"t":"pending","id":"u1","need":"pin"}
                            ← {"t":"unlocked","id":"u1"}
                               | {"t":"denied","id":"u1","reason":"wrong_pin"|"wiped"|"timeout"|"policy"}
```

The daemon locks the controller when presence is lost (owner out of sight, robot lifted); the controller
also boots locked. Unlocking needs the PIN rhythm, verified inside the SE050 (`need:"touch"` only before
`set_pin` has run). A wrong rhythm increments the chip's counter and `pin_failures`; the fifth wrong
rhythm answers `denied:wiped`, followed by `{"t":"event","ev":"wiped"}`: flash is zeroed and the key and
PIN objects are deleted in the chip. The device is then uninitialised; the 24 words are the only recovery.
`policy` denotes a link error (the chip could not be asked); it does not count as a failure.

Every `unlock` rhythm is checked against **both** objects, in the same order and with the same number of
rounds every time (so neither timing nor the controller–chip traffic says which one matched). The real
rhythm unlocks the real wallet and clears a voice-command duress. The second rhythm (`set_duress_pin`)
answers **exactly the same** `{"t":"unlocked","id":…}`, shows the same screen, emits no event and leaves
`pin_failures` untouched, but from then on the decoy slot is active: `hello.addresses` are the decoy's, the
decoy keys sign, the allowance is gone, and every owner-authority request (`policy`, `elevate`, `allowance`,
`inheritance_*`, `set_duress_pin`, `decoy_addresses`) is refused with `policy`, exactly as after the voice
`duress` command. The real rhythm at the next `unlock` returns to the real wallet. The daemon cannot tell
the two unlocks apart and must not try to; the beacon is not pulsed on this path.

```json
→ {"t":"decoy_addresses","id":"x1"}
← {"t":"pending","id":"x1","need":"pin"}
← {"t":"decoy_addresses","id":"x1","addresses":{"solana":"<base58>","evm":"0x…","bitcoin":"bc1q…"}}
   | {"t":"denied","id":"x1","reason":"wrong_pin"|"policy"|"pin_not_set"|"locked"|"busy"|"timeout"}
```

The decoy wallet's public keys, for funding it, after the real rhythm; refused (`policy`) while the decoy
slot is active, like every other owner-authority request.

### policy

```json
→ {"t":"policy","id":"p1","daily_limit_lamports":"5000000000","daily_limit_wei":"2000000000000000000","daily_limit_sats":"300000"}
← {"t":"pending","id":"p1","need":"pin"} ← {"t":"policy","id":"p1","ok":true}
   | {"t":"denied","id":"p1","reason":"wrong_pin"|"wiped"|"pin_not_set"|…}
→ {"t":"policy","id":"p2","add_router":{"chain":"evm","address":"0x…"}}        // or "remove_router":{…}, chain "evm"|"solana"
← {"t":"pending","id":"p2","need":"pin"} ← {"t":"policy","id":"p2","ok":true}
```

Any subset of the three limits and/or one router change (at least one field); limits not named are left
alone. Policy changes need the PIN rhythm (they used to need a long touch). All three limits start at 0
after `init`: nothing leaves on a chain until its limit is set. The spend counters of all chains reset
together at the day rollover (see `time`). Router changes: up to 8 owner entries beside the built-in set
(`hello.routers`), persisted in the controller's flash; the display shows the chain and the full address
while the rhythm is tapped and again after the change; built-in entries cannot be removed.

### time

```json
→ {"t":"time","unix":1791600000}        // no reply when accepted; on rejection:
← {"t":"event","ev":"time_rejected"}
```

Host time is untrusted and is used only for the daily-counter rollover and allowance expiry. The first value
after boot is accepted; afterwards a value lower than the last accepted one, or more than 36 hours ahead of
the controller's interpolated clock, is ignored (`time_rejected`) and the last accepted value stays. The
daily counters reset only when **both** hold: the host's unix day differs from the stored one, and the
controller has been powered for at least 20 hours since the previous rollover (its own uptime counter, the
one the inheritance timer uses). A day change without the uptime leaves the counters and the stored day
untouched until the uptime is there. Spent counters never reset on host time alone.

### events (controller → daemon, unsolicited)

```json
{"t":"event","ev":"touch"}            // any touch, for UI feedback
{"t":"event","ev":"long_touch"}
{"t":"event","ev":"tamper","what":"usb_reset"|"brownout"}
{"t":"event","ev":"rhythm","what":"too_few"}   // fewer than 4 taps before the pause; try again
{"t":"event","ev":"pin_failed"}                // a wrong rhythm was counted (hello shows the total)
{"t":"event","ev":"wiped"}                     // fifth failure: device reset to factory state
{"t":"event","ev":"inheritance_warning","days_left":7}   // inheritance timer crossed a warn_days threshold
{"t":"event","ev":"display","page":2,"pages":3}          // the trusted display now shows page 2 of 3
{"t":"event","ev":"time_rejected"}                       // a `time` value was ignored (backwards or > 36 h ahead)
```

`denied` reasons, complete list: `timeout`, `claims_mismatch`, `over_daily_limit`, `locked`, `policy`,
`user` (also: the owner held the pad 3 s to cancel), `busy`, `not_initialized`, `pin_not_set`, `wrong_pin`,
`pin_mismatch`, `wiped`, `not_armed`, `not_expired`, `unsupported_typed_data`, `not_supported`.

### duress

```json
→ {"t":"duress","id":"d1"}   ← {"t":"duress","id":"d1","ok":true}
```

Switches the controller to the decoy key slot until the next `unlock` with the real rhythm, and pulses the
beacon line. The real key slot refuses every request while duress is active. The daemon sends this when the
robot recognises the duress word or expression; because the daemon is untrusted, duress is a *protection*,
never a way to reach the real key. The silent way in is the second rhythm (`set_duress_pin` / `unlock`):
no command, reply, event or `hello` field reports whether the decoy slot is active.

## 2. Daemon ⇄ robot (ws://127.0.0.1:7402)

Robot → daemon state, pushed on change and every 2 s:

```json
{"t":"presence","person":true,"owner":true,"voice_ok":false,"duress":false,"moved":false,
 "confidence":{"lidar":0.92,"face":0.88},"ts":1791600000}
```

Daemon → robot requests:

```json
{"t":"say","id":"x1","text":"0.5 SOL to an address created three days ago. Touch to sign."}
{"t":"ask_passphrase","id":"x2"}      ← {"t":"passphrase","id":"x2","ok":true}
{"t":"attention"}                     // look at the owner, step forward
{"t":"morning_report","items":[…]}    // spoken newspaper
{"t":"display","id":"x3","title":"0.5 SOL","body":"to 9xQe…abcd · wallet 3 days old","level":"low|medium|high|block","ttl_secs":60}  // front panel
```

Optional (robot answers `error` if `PINCER_BRAIN=none`, the default; the daemon must not depend on it):

```json
{"t":"chat","id":"c1","text":"what did I miss?"}   ← {"t":"chat","id":"c1","text":"Two trades closed. Nothing is pending."}
```

The robot replies with the text first, then speaks it. The brain (Claude or a local model on the Jetson)
sees only whitelisted presence fields (`person`, `owner`, `voice_ok`, `moved`, `duress`, time, owner
name), never a key, hash or passphrase, and its output is filtered: secret-shaped tokens are redacted
and any "I signed it" claim is rewritten. `chat` is companionship, not a control: nothing it says is an
input to the signing path.


Additional robot → daemon states (room lock, two-person rule, air-gap):

```json
{"t":"presence", …, "room_match":0.97, "persons":2, "owners":["<pubkey-label>","spouse"], "network":false}
```

- `room_match`: similarity of the current lidar scan to the enrolled room fingerprint (0–1). The daemon
  refuses signatures below the configured threshold ("this is not my room").
- `persons`: distinct human bodies the lidar sees; `owners`: enrolled faces matched right now. The
  two-person rule needs `persons ≥ 2` and two distinct enrolled owners.
- `network`: whether the robot currently has its radio enabled (air-gap relay).

Daemon → robot:

```json
{"t":"network","on":true,"reason":"owner_present"|"agent_window"|"broadcast","for_secs":600}
{"t":"enroll_room","id":"r1"}      ← {"t":"enroll_room","id":"r1","ok":true}
```

All of these features are **off by default** and switched at runtime (kept in the robot's memory, applied
immediately; `PINCER_ROOM_LOCK` / `PINCER_TWO_PERSON` / `PINCER_AIRGAP` / `PINCER_WAKE` give the boot
defaults). Keys may be omitted; the reply is the full current set:

```json
{"t":"settings","id":"s1","room_lock":true,"two_person":false,"airgap":"off"|"strict"|"night","wake":true}
                                   ← {"t":"settings","id":"s1","room_lock":true,"two_person":false,"airgap":"off","wake":true}
```

- `room_lock` publishes `room_match` (fingerprint from the lidar background, or map localisation when a map
  exists; see below). `enroll_room` answers `ok:false` while the lidar is still learning its background
  (first 30 s after start).
- `two_person` publishes `persons` and `owners`. `owner` (singular) is always the primary owner; `owners`
  lists every enrolled label (`pincer-enroll --add-person <label>`) matched within the last 3 s.
- `airgap`: `off` = relay always on, daemon `network` requests ignored. `strict` = radio only while the
  owner is present plus 60 s; a `network on` request is ignored, `network off` is honoured for `for_secs`.
  `night` = strict plus daemon-requested windows (`network on … for_secs`). `network` in `presence` is the
  state actually applied to the relay (omitted when `airgap` is `off`). The robot leaves the radio on when
  the service exits.
- `wake` enables the wake-up alarm below; while it is off every `wake` request is answered at once with
  `acked:false,"reason":"wake_off"`.

**wake** — a wake-up alarm the daemon raises for an event the owner must hear now (e.g. `PORTFOLIO_DROP`).
Touches reach the robot only through the daemon, so the daemon forwards the controller's touch event as
`{"t":"touch"}`:

```json
{"t":"wake","id":"w1","reason":"PORTFOLIO_DROP","lines":["Wake up.","SOL is down 20 percent."],"max_secs":120}
{"t":"touch"}                                      // any touch while a wake is running acknowledges it
                                   ← {"t":"wake","id":"w1","acked":true,"secs":37,"reason":"touch","travelled":true,"room":"bedroom"}
```

The robot (1) if the owner is not currently in view and a map with named rooms exists, goes looking for
them (`find_owner`, bounded by 60 % of `max_secs`), (2) turns toward the owner's last known bearing and
walks forward 20 cm, (3) speaks `lines` in order, each delivery more urgent than the last (ElevenLabs
`stability` lowered / espeak-ng amplitude and rate raised) with shrinking pauses, repeating until a
`touch`, the passphrase is heard by the continuous listener, or `max_secs` elapse. Reply fields: `acked`,
`secs` (elapsed), `reason` = `touch` | `passphrase` | `timeout` | `wake_off` | `busy` (a wake is already
running), `travelled` (navigation moved the robot), `room` (where it found the owner, or `null`). Without
`lines` the robot says `"Wake up. <reason>."`. A `say` sent during a wake is spoken between the alarm
lines; it does not stop the alarm.

**Mapping and navigation** (slam_toolbox + Nav2 on the robot; a teleporting mock on a laptop):

```json
{"t":"map","id":"m1","action":"start"}            // slam_toolbox mapping; drive/carry the robot around
{"t":"map","id":"m2","action":"save","name":"home"}   // map_saver → ~/.pincer1/maps/home.{yaml,pgm}, stops mapping
{"t":"map","id":"m3","action":"status"}
                                   ← {"t":"map","id":"m3","action":"status","ok":true,"mapping":false,"map":true,
                                      "map_name":"home","rooms":["bedroom","desk"],"pose":{"x":1.2,"y":4.0,"yaw":0.1},
                                      "room":"bedroom","localized":true}
{"t":"name_room","id":"n1","name":"bedroom"}     // the owner stands there: stores the current map pose + 2 m radius
{"t":"name_room","id":"n2","name":"living","radius":3}
{"t":"name_room","id":"n3","name":"hall","polygon":[[0,0],[3,0],[3,1.2],[0,1.2]]}   // map-frame metres
                                   ← {"t":"name_room","id":"n1","ok":true,"name":"bedroom","x":1.2,"y":4.0,"radius":2.0,"polygon":null,"rooms":[…]}
{"t":"goto","id":"g1","room":"bedroom"}          ← {"t":"goto","id":"g1","room":"bedroom","ok":true,"reason":"arrived","secs":41}
```

`goto` reasons: `arrived`, `timeout` (90 s default), `no_map`, `unknown_room`, `blocked`/`rejected`
(Nav2 result). Rooms live in `~/.pincer1/rooms.json`. When a map exists, `presence` carries
`"room":"<name>|null"` (where the robot thinks it is), and `room_match` switches from the lidar
fingerprint to localisation: `1.0` when AMCL/slam has converged (pose covariance below
`PINCER_NAV_LOCALIZED_COV`, 0.25 m²) inside a named room or the mapped area, `0.0` when it has not
converged or the pose is nowhere known. `find_owner` visits the room where the owner was last seen, then
`bedroom`, `desk`, `living` (`PINCER_NAV_ROOM_PRIORITY`), then the rest, turning through 360° and watching
for a person cluster + face for 10 s at each stop.

**Battery and charging dock** (optional feature `dock`, off by default; the battery fields are always there):

```json
{"t":"presence", …, "battery":{"pct":42,"voltage":11.52,"charging":false}, "dock":"idle"}
```

- `battery.pct`: state of charge 0–100 from a 3S open-circuit-voltage table on a 2 s low-passed pack
  voltage (approximate under load, pessimistic while walking). `voltage` in volts, `charging` = current
  into the pack ≥ 0.3 A (also the only "I am on the dock" signal; the plate has no electronics).
- `dock`: `off` (feature disabled) | `idle` | `navigating` | `approaching` | `verifying` | `charging` |
  `undocking` | `failed`. A change of `dock` or of `charging` is pushed at once; `pct`/`voltage` ride on
  the heartbeat.

Enrolment: with a map loaded, put the robot on the dock (rear to the wall, brick plugged in) and send
`{"t":"name_room","name":"dock"}`. The reply gains a `dock` object (on-dock pose, `wall_m` = lidar range to the
wall behind); the registered room `dock` is the staging pose 0.5 m in front of the plate, radius 0.3 m:

```json
{"t":"name_room","id":"n4","name":"dock"}
                                   ← {"t":"name_room","id":"n4","ok":true,"name":"dock","x":1.7,"y":4.0,"radius":0.3,"polygon":null,
                                      "rooms":[…],"dock":{"x":1.2,"y":4.0,"yaw":0.0,"wall_m":0.25}}
```

Daemon → robot:

```json
{"t":"settings","dock":true}                      // same message as the other features; PINCER_DOCK boot default
{"t":"dock","id":"d1","action":"status"}          ← {"t":"dock","id":"d1","action":"status","ok":true,"state":"charging",
                                                      "enabled":true,"enrolled":true,"map":true,"attempts":1,
                                                      "return_pending":false,"wall_m":0.25,
                                                      "battery":{"pct":63,"voltage":11.9,"charging":true},"last_error":null}
{"t":"dock","id":"d2","action":"go"}              ← {"t":"dock","id":"d2","action":"go","ok":true,"reason":"queued", …status}
{"t":"dock","id":"d3","action":"leave"}           ← {"t":"dock","id":"d3","action":"leave","ok":true,"left":true, …status}
```

`go` docks now regardless of the state of charge (and of `busy`); `ok:false` with `reason` `dock_off` |
`not_enrolled` | `no_map`. `leave` undocks (0.3 m forward) or abandons a docking run; the robot does **not**
return on its own after a daemon `leave`. Unknown actions get `error:"bad_action"`.

Behaviour: at ≤ 20 % (`PINCER_DOCK_AT_PCT`), when enabled, enrolled, a map exists and nothing has priority
(no wake running, no speech / passphrase in progress, no `attention` in the last 30 s, body not moving), the
robot navigates to `dock`, backs up 0.5 m at 0.05 m/s on the rear lidar range until the enrolled wall distance
or current, waits up to 10 s for `charging`, retries after a 0.2 m back-off (3 attempts), then gives up: it says
so, `dock:"failed"`, and does not retry alone for 15 min. It charges until ≥ 90 % (`PINCER_UNDOCK_AT_PCT`),
then walks 0.3 m forward. A `wake` and an `attention` pull it off the dock first (the wake reply is unchanged);
afterwards it returns on its own once free, if still below 90 %.

Robot → daemon events (unsolicited; a daemon that does not know them can ignore them):

```json
{"t":"event","ev":"dock_failed","reason":"no_current"|"nav:<goto reason>"|"error:…","attempts":3}
{"t":"event","ev":"battery_low","pct":15,"dock":"off"|"failed"|"idle"}
```

`battery_low` fires once at ≤ 15 % (`PINCER_BATTERY_LOW_PCT`) whenever the robot cannot help itself — docking
disabled, failed, or not enrolled / no map — and re-arms after the pack recovers above 20 % or charges. That is
the daemon's cue to phone the owner.

## 3. Extension ⇄ daemon (ws://<robot>:7401)

Pairing: the daemon shows a 6-digit code (spoken by the robot); the extension sends it once and receives a
token stored in extension storage.

```json
→ {"t":"pair","code":"123456"}                       ← {"t":"paired","token":"…","pubkey":"<base58>"}
→ {"t":"connect","token":"…","origin":"https://pump.fun"} ← {"t":"connected","pubkey":"<base58>"}
→ {"t":"signTransaction","id":"t1","tx":"<base64 serialized tx>","origin":"https://pump.fun"}
← {"t":"explain","id":"t1","summary":"…","risk":"low"|"medium"|"high"|"block","reasons":[…]}
← {"t":"signed","id":"t1","tx":"<base64 signed tx>"}  |  {"t":"denied","id":"t1","reason":"…"}
→ {"t":"signMessage","id":"m1","message":"<base64>"}   ← {"t":"signed","id":"m1","signature":"<base64>"}
```

The daemon never forwards a request to the controller before it has (a) parsed and explained it, (b) run the
scam filter (`block` ⇒ denied without asking), (c) confirmed presence, (d) had the robot speak the summary.

### Multi-chain extension

`signTransaction` and `signMessage` take `chain` (`"solana"` default, `"evm"`, `"bitcoin"`). The payload field
depends on the chain; `explain` carries `chain` back and `lines` for the popup.

```json
→ {"t":"signTransaction","id":"e1","chain":"evm","chainId":8453,"tx":"<base64 unsigned RLP, legacy or EIP-1559>","origin":"https://app.example"}
← {"t":"explain","id":"e1","chain":"evm","summary":"On Base: Sends 0.25 ETH to 0x1234…1234, a wallet with 12+ transactions.","lines":[…],"risk":"low","reasons":[…]}
← {"t":"signed","id":"e1","chain":"evm","tx":"0x<signed RLP hex>"}  |  {"t":"denied","id":"e1","reason":"blocked"|"parse_error"|"no_key"|…,"detail":"…"}

→ {"t":"signTransaction","id":"b1","chain":"bitcoin","psbt":"<base64 BIP-174 PSBT>"}
← {"t":"signed","id":"b1","chain":"bitcoin","tx":"<final raw tx hex>","txid":"…"}      the daemon finalised; the dapp (or POST /broadcast) sends it

→ {"t":"signMessage","id":"m2","chain":"evm","message":"<base64 EIP-191 bytes>"}
→ {"t":"signMessage","id":"m3","chain":"evm","chainId":1,"typedData":{…EIP-712 JSON…}}
← {"t":"signed","id":"m3","chain":"evm","signature":"0x<65 bytes r‖s‖v, v = 27/28>"}
```

`chainId` on an EVM request is what the dapp claims to be connected to; a transaction (or typed-data domain)
for another chain is blocked. Unknown `chain` → `denied:bad_chain`. A Bitcoin request without `psbt`, or an
EVM request whose bytes are already signed, → `denied:parse_error`. `no_key` means the controller reported no
address for that chain (`hello.addresses`). Bitcoin message signing (BIP-322) is not offered yet
(`denied:unsupported`).

### 3.1 Multi-chain payloads (what the extension sends since 0.2.0)

Every `signTransaction` / `signMessage` now carries `chain: "solana" | "evm" | "bitcoin"`; a daemon that
predates this field can treat a missing `chain` as `"solana"`. `connected` (and optionally `paired`) may
carry `addresses`; the extension uses `pubkey` for Solana when `addresses.solana` is absent, and reports
"the robot exposes no evm/bitcoin address" to dapps when those keys are missing.

```json
← {"t":"connected","pubkey":"<base58>",
   "addresses":{"solana":"<base58>","evm":"0x<20 bytes>","bitcoin":"bc1q…","bitcoinPublicKey":"02…"}}

→ {"t":"signTransaction","id":"t1","chain":"solana","tx":"<base64 serialized tx>","origin":"https://pump.fun"}
← {"t":"signed","id":"t1","tx":"<base64 signed tx>"}

→ {"t":"signTransaction","id":"e1","chain":"evm","chainId":8453,"tx":"0x02…","origin":"https://app.uniswap.org"}
← {"t":"signed","id":"e1","tx":"0x02…"}

→ {"t":"signTransaction","id":"b1","chain":"bitcoin","psbt":"<base64>","signInputs":{"bc1q…":[0,1]},
   "finalize":true,"origin":"https://magiceden.io"}
← {"t":"signed","id":"b1","psbt":"<base64 signed psbt>","tx":"<raw tx hex, only if finalize and complete>"}

→ {"t":"signMessage","id":"m1","chain":"solana","message":"<base64>","origin":"…"}
← {"t":"signed","id":"m1","signature":"<base64>"}

→ {"t":"signMessage","id":"m2","chain":"evm","message":"0x<bytes>","origin":"…"}          personal_sign
← {"t":"signed","id":"m2","signature":"0x<65 bytes>"}

→ {"t":"signMessage","id":"m3","chain":"evm","typedData":{"types":{…},"primaryType":"Permit",
   "domain":{…},"message":{…}},"origin":"…"}                                                  eth_signTypedData_v4
← {"t":"signed","id":"m3","signature":"0x<65 bytes>"}

→ {"t":"signMessage","id":"m4","chain":"bitcoin","message":"<utf-8>","address":"bc1q…","origin":"…"}   BIP-322
← {"t":"signed","id":"m4","signature":"<base64>"}
```

Field notes:

- **evm `tx`** is the RLP of the *unsigned* transaction: typed `0x02` (EIP-1559) or, when the dapp passed
  `gasPrice` or the chain reports no `baseFeePerGas`, an EIP-155 legacy list `[nonce, gasPrice, gas, to,
  value, data, chainId, 0, 0]`. `chainId` is repeated at the top level so the daemon can refuse to sign for a
  chain it does not know without decoding. Nonce, gas and fees were already filled in by the extension from
  the user-configured RPC. The reply `tx` is the signed RLP; the extension re-parses it and refuses to
  broadcast if chainId, nonce, to, value or data differ from what it sent.
- **evm `message`** is the raw message bytes as hex; the signer applies the EIP-191 `\x19Ethereum Signed
  Message:\n<len>` prefix. `typedData` is the EIP-712 object exactly as the dapp supplied it (JSON-parsed).
  `eth_sign` (raw 32-byte hash signing) is never sent: the extension refuses it with EIP-1193 code 4200.
- **bitcoin `psbt`** is base64. `signInputs` (optional) maps an address to the input indexes the dapp wants
  signed, sats-connect style; absent means "every input you own". `finalize: true` means the dapp asked for a
  broadcast: the daemon should finalize if every input is now complete and return the raw transaction as
  `tx` (hex). The extension broadcasts `tx` itself (`POST {esplora}/tx`) and never parses PSBTs.
- **explain** may add `lines: string[]`, the sentences the robot spoke; the popup shows them under the
  reasons.

The `origin` is always the content-script port's origin, never page-supplied, as before.

## 4. Daemon HTTP (127.0.0.1:7401, same port, plain GET/POST)

```
GET  /status            → {pubkey, controller:{fw,se050,locked,policy}, presence, agent}
POST /explain {tx}      → {summary, risk, reasons, parsed}
GET  /agent             → {budget, open, pnl, report}
POST /agent/budget      → {lamports}
GET  /agent/report      → the last morning report
```

```
POST /offline/sign {tx, origin?}         → {signedTx, qrSvg, id}   signs now (presence + touch), does not broadcast
GET  /offline/pending                    → [{id, summary, signedAt, usesDurableNonce}]
POST /offline/broadcast {id}             → {signature}            when the network is back (or paste the QR elsewhere)
POST /offline/nonce/create               → {nonceAccount}         one-time: creates a durable-nonce account for offline use
GET  /inheritance                        → status from the controller + configured heir
POST /inheritance/arm {heir, inactivityDays, warnDays, solLamports?, mints?}  → builds the durable-nonce tx, long touch, stored on the controller
POST /inheritance/clear                  → long touch
POST /inheritance/release                → broadcasts if the controller says the timer expired
```

Multi-chain and swaps:

```
GET  /status                              → {pubkey, addresses:{solana,evm,bitcoin}, chains:[…], mode:"online"|"offline", brain, controller:{…policy incl. daily_limit_wei/daily_limit_sats}, presence, agent}
POST /explain {chain?, tx|psbt, chainId?, origin?}  → {chain, summary, lines, risk, reasons, parsed:{nativeOut, nativeUnit, dest, programs, dangers, tokenOut}}
POST /broadcast {chain, tx}               → {chain, txid}     tx: base64 (solana) or hex (evm, bitcoin)
GET/POST /settings                        → adds mode ("online"|"offline"), brain ("lamport"|"claude"|"openai"|"deepseek"),
                                            unlimitedApproveToRouters, swapMaxSlippageBps (≤ 100).
                                            POST → 400 {error:"offline_mode", detail} when a cloud brain is chosen in offline mode
GET  /briefing                            → {facts:{date, quotes:[{id,symbol,usd,change24h}], headlines:[{title,source,link,at}], portfolio?, agent?, offlinePending?, inheritance?, battery?, mode, notes}, lines:[…]}
POST /briefing/now                        → reads it now (same body); the schedule is settings.briefing {enabled, time "HH:MM", coins (CoinGecko ids), topics, feeds, maxHeadlines, portfolio, requirePresence}
POST /swap/quote {from:{chain,asset,chainId?}, to:{chain,asset,chainId?}, amount, slippageBps?}
                                          → {quoteId, route:"jupiter"|"1inch"|"0x"|"thorchain", from, to, amountIn, expectedOut, minOut, slippageBps, spoken, expiresAt}
                                            asset: "SOL" | mint, "ETH"/"BNB"/"POL" | 0x…, "BTC"; amount is a human decimal string
POST /swap/execute {quoteId}              → {ok, quoteId, steps:[{kind:"approve"|"swap", chain, txid}], spoken}
                                            403 {error:"offline_mode"} in offline mode; 400 {error:"router"|"min_out"|"slippage"|"amount"|"receiver"|"expired"|…}
                                            when the router's bytes fail verification (nothing is signed in that case)
```

Swap verification happens before the controller is asked: the built transaction's `to`/program must be the
allowlisted router (Jupiter v6, 1inch v6, 0x Exchange Proxy, or THORChain's router/inbound vault pinned for
10 minutes), its decoded minimum output must be ≥ quote × (1 − slippage), slippage ≤ 1 %, and any ERC-20
approval is exact-amount to the router. The signature then goes through the ordinary §3 pipeline (presence,
spoken explanation, touch, controller re-parse) and the daemon broadcasts.

## 5. Beacon (locator, optional)

The beacon (`beacon/`) is a separate board with its own cell, GNSS, BLE and LoRa, hidden in the body.
It is not on any of the links above: it has three input lines and one radio link to the owner's base
station. It reveals the robot's location, never a key; nothing on it can sign.

### Trigger lines

| line | from | level | meaning |
|---|---|---|---|
| `TRIG_CTRL` | consent controller GP16 (`PIN_BEACON`) | 3.3 V, high for 2 s | duress, tamper (`usb_reset`, brownout) or the PIN wipe. **Trusted**: counts even when the beacon is idle. |
| `TRIG_ROBOT` | a Jetson GPIO driven by `pincer_robot` | 3.3 V, high ≥ 500 ms | `moved` with no owner in sight. **Untrusted**: worst case a false alarm. Counts only when armed, and not for 30 min after the owner's `ok`. |
| `TAMPER` | normally-closed switch on the shell, to GND | pulled up; high when opened or cut | shell opened. Counts when armed. |

Plus the beacon's own IMU (sustained motion for 5 min while armed, same 30-min grace as `TRIG_ROBOT`)
and a one-hour heartbeat while armed. States: `idle` → `armed` → `alarm`; only an authenticated
command over LoRa ends an alarm.

### LoRa packet

One fixed 48-byte frame in both directions, ChaCha20-Poly1305 (RFC 8439) under the 32-byte key from
`beacon/tools/provision.py`, nonce = device id (8) ‖ seq (4); the header is authenticated, the body
encrypted. Little-endian.

```
header (clear, authenticated)   0 version=1 · 1..8 device id · 9 type · 10..13 seq (bit 31 = direction: 0 beacon→base, 1 base→beacon)
body (encrypted, 18 bytes)      battery_mv u16 · lat i32 1e-7° · lon i32 1e-7° · fix_age_s u16 (0xFFFF never) ·
                                trigger bits u8 (1 ctrl · 2 robot · 4 tamper · 8 motion · 16 low battery) · hdop_x10 u8 (0xFF unknown) ·
                                findmy_idx u16 · state u8 (0 idle · 1 armed · 2 alarm) · cmd u8
tag                             16 bytes
```

| type | direction | when |
|---|---|---|
| `heartbeat` (1) | beacon → base | hourly while armed; battery and a coarse (0.01°) position, or none if `BEACON_HEARTBEAT_POSITION=0` |
| `alarm` (2) | beacon → base | at once on alarm (last known position), again on the first fix or after 90 s, then every 2 min for an hour, every 10 min after |
| `ack` (3) | beacon → base | after every accepted command: `cmd` = what was acknowledged, `state` = the state now |
| `cmd` (4) | base → beacon | `cmd` = 1 `arm` · 2 `disarm` · 3 `ping` · 4 `ok` (owner is moving the robot: soft triggers ignored 30 min) |

Replay protection: each side keeps its own counter (persisted; the beacon reserves blocks of 64 in
flash) and accepts from the peer only a seq strictly greater than the last accepted one. A recorded
`disarm` cannot be replayed. Lost packets are fine (gaps are allowed, going backwards is not).

Listening: idle and armed, the beacon listens for 3 s every 15 min and for 3 s after each heartbeat;
in alarm it listens continuously between packets. The base therefore repeats a command every 2 s
until the `ack` arrives (default 20 min).

### Base station serial (USB, 115200, one JSON object per line)

Base → host: `{"t":"alarm"|"heartbeat"|"ack","id":"<hex>","seq":n,"battery_mv":n,"lat":f|null,"lon":f|null,
"fix_age_s":n|null,"trig":["ctrl",…],"hdop":f|null,"findmy_idx":n,"state":"armed","cmd":"disarm","rssi":f,"snr":f,"ms":n}`,
`{"t":"reject","reason":"auth"|"replay"|"id"|"dir"|"len"|"version"|"type"|"radio",…}`,
`{"t":"sent"|"sending"|"gave_up"|"stopped"|"base"|"error",…}`.

Host → base: `{"cmd":"arm"|"disarm"|"ping"|"ok"|"stop","repeat_s":n}` or `status`.

`beacon/base/relay.py` consumes these and forwards to ntfy / Telegram / a webhook, and takes
commands over `POST 127.0.0.1:7410/cmd`, `relay.py send <cmd>`, or Telegram (opt-in).

### Find My

In alarm the beacon advertises Apple offline-finding frames (OpenHaystack scheme) with a P-224 key
that rotates every 15 min; the key index rides in every LoRa packet. `beacon/tools/findmy_keys.py`
recreates the key list from the seed for the fetcher. Derivation and honest status in
`beacon/src/findmy.h` and `beacon/README.md`.
