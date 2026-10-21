# Pincer1 consent controller firmware

The only trusted software in Pincer1. Runs on a Raspberry Pi **Pico 2 (RP2350, Arm Cortex-M33, secure
mode)** wired to the NXP SE050 secure element and the touch pad. It speaks newline-delimited JSON over USB
to the daemon (`docs/PROTOCOL.md` §1), re-parses every Solana message, EVM transaction and Bitcoin PSBT
itself, and asks the SE050 to sign only after a physical touch and only when the daemon's claims about the
transaction hold.

v1 adds three things against physical seizure of the robot:

- **Secure boot.** The image is signed (secp256k1, `picotool seal`); the RP2350 boots only images whose
  signing key hash is in its OTP. A reflashed controller does not run.
- **SE050 platform binding (SCP03).** Every APDU between controller and chip is encrypted and MACed with
  keys that live in the controller's OTP. A desoldered SE050 or a swapped controller cannot talk to it.
- **PIN inside the chip.** A tap rhythm on the head (4–8 taps, short/long gaps) is verified by the SE050
  as a UserID authentication object with a 5-attempt limit; the signing keys carry a policy that allows
  signing only inside a session authenticated with it. Five wrong rhythms wipe the device.

v0.2 adds **multi-chain custody**: one 24-word seed, three key families inside the SE050 (ed25519 for
Solana, secp256k1 for EVM chains and for Bitcoin native segwit), each with its own on-device parser,
claims check and daily limit. See "Multi-chain custody" below.

v0.3 closes the gap between what the owner perceives and what gets signed:

- **Trusted display.** A 128×64 OLED driven by this firmware alone shows every request from the
  controller's own parse (chain, amount in native units, destination, token moves, approvals and unknown
  contracts as boxed warnings, fee, chain id). A request becomes `pending(touch)` only after its screen
  has been rendered, and a touch counts as consent only once the last page has been shown. The daemon's
  spoken summary is a convenience; the display is the consent channel. See "Trusted display".
- **The seed never leaves the device.** `init` shows the 24 words (and the decoy set) on the display, four
  per page, twice each; the USB reply carries public keys only. `reveal_seed` is refused by design.
- **Host time is untrusted.** The daily counters reset only when the host's day changed AND the controller
  has been powered for 20 hours since the last reset; backwards time and jumps of more than 36 hours are
  ignored (`event:time_rejected`).
- **Destinations are compared**, not only amounts: Solana recipient and token recipients, ERC-20 recipients,
  Bitcoin destination must equal what the daemon claimed.
- **The swap-router allowlist lives here**, not in `claims.routers`: Jupiter v6, 1inch v6 and 0x built in,
  up to 8 owner-added entries through the PIN-gated `policy`, each shown on the display when added.

v0.4 adds a **second rhythm** (`set_duress_pin`): an `unlock` with it answers and looks exactly like a real
unlock but activates the decoy wallet, with no event, no counter change and nothing in `hello` that the
daemon could use to tell. The `init` reply carries the decoy's public keys once (`decoy_addresses`, also
readable later under the real PIN) so the owner can fund it. See "PIN rhythm".

And one thing for the owner's heirs:

- **Inheritance dead-man's switch.** A durable-nonce transaction to the heir is signed once (long touch),
  stored in its own flash region, and released as a wire transaction only after the robot has been
  powered and untouched for `inactivity_days`. The timer is powered uptime counted from the 5 ms tick and
  persisted hourly (never host time); any touch resets it. `lock`, `duress` and the PIN wipe do not clear
  it, only `inheritance_clear` with a long touch does. See "Inheritance" below and `docs/PROTOCOL.md` §1.

## Wiring

Identical to the Pico: same GPIO numbers, same footprint.

| Pico 2 | signal | to |
|---|---|---|
| GP4 | I2C0 SDA | SE050 SDA |
| GP5 | I2C0 SCL | SE050 SCL |
| 3V3 | power | SE050 VCC + ENA, TTP223 VCC |
| GND | ground | SE050 GND, TTP223 GND |
| GP15 | touch (active high) | TTP223 OUT |
| GP17 | SPI0 CS | OLED CS |
| GP18 | SPI0 SCK | OLED SCK (D0) |
| GP19 | SPI0 MOSI | OLED MOSI (D1) |
| GP20 | GPIO out | OLED D/C |
| GP21 | GPIO out | OLED RES |
| 3V3 / GND | power | OLED VCC / GND (2.42" 128×64 SSD1309, 4-wire SPI) |
| GP25 | on-board LED | heartbeat idle · slow blink touch · fast blink long touch · double blink PIN · solid while touched |
| GP26 | ADC0, left floating | extra entropy for the RNG |
| USB | CDC serial | Pi 5 (`/dev/ttyACM0`) |

Full details and placement in `docs/HARDWARE.md`.

## Build

Requires CMake ≥ 3.13, picotool 2.x on `PATH` (the SDK uses it to seal/sign; 2.3.1 tested) and the Arm GNU
toolchain (`arm-none-eabi-gcc` **with newlib**; the Homebrew `arm-none-eabi-gcc` formula ships without a C
library and fails at link time, use the official tarball from developer.arm.com or the `gcc-arm-embedded`
cask). The pico-sdk is fetched automatically (`PICO_SDK_FETCH_FROM_GIT`, master = 2.3.1 at the time of
writing; RP2350 needs a 2.x SDK and the CMake refuses older ones), or set `PICO_SDK_PATH`.

```bash
git submodule update --init firmware/third_party/plug-and-trust    # NXP Plug & Trust middleware v04.08.02

# production: SE050 signer, Pico 2, unsigned image (bring-up)
cmake -S firmware -B build-pico2 -DPICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain/bin
cmake --build build-pico2 -j8          # → build-pico2/pincer1_consent.uf2  (628 736 bytes; text 318 KB, bss 58 KB)

# production, signed for secure boot (see "Signing")
cmake -S firmware -B build-signed -DPICO_TOOLCHAIN_PATH=… -DPINCER_SIGN_KEY=firmware/keys/pincer1-boot.pem
cmake --build build-signed -j8         # → pincer1_consent.uf2 (signed+hashed) + pincer1_consent.otp.json

# dev boards without the chip: software ed25519 signer, hello reports se050:false
cmake -S firmware -B build-soft -DPINCER_SOFT_SIGNER=ON -DPICO_TOOLCHAIN_PATH=…
cmake --build build-soft -j8           # → build-soft/pincer1_consent.uf2  (568 320 bytes; text 288 KB, bss 56 KB)

# bench with an unrotated SE050: compile the chip's current platform keys in (never for production)
cmake -S firmware -B build-bench -DPINCER_DEV_SCP03_KEYS=<96 hex ENC||MAC||DEK> -DPICO_TOOLCHAIN_PATH=…

# bench board without the OLED: display bus calls become no-ops, hello reports display:false (never for production)
cmake -S firmware -B build-nodisp -DPINCER_NO_DISPLAY=ON -DPICO_TOOLCHAIN_PATH=…
cmake --build build-nodisp -j8         # → build-nodisp/pincer1_consent.uf2 (619 520 bytes; text 314 KB, bss 58 KB)
```

`PINCER_NO_DISPLAY` keeps every screen rendered into the framebuffer and keeps the consent machine's
"screen shown" step, so the logic under test is the production logic; only the SPI traffic is dropped.
A production image must be built without it: without the panel the owner has no trusted view of what is
being signed, and the daemon refuses production mode when `hello` says `display:false`.

`PICO_BOARD` defaults to `pico2` (`PICO_PLATFORM=rp2350-arm-s`). `-DPICO_BOARD=pico` still configures for the
original Pico (not re-measured since v0.1) but that part has no OTP and no secure boot: SCP03 keys can only come from
`PINCER_DEV_SCP03_KEYS` and `PINCER_SIGN_KEY` is refused. It is kept as a compile check only.

Flash: hold BOOTSEL, plug the board in, copy the `.uf2` to the `RP2350` drive (or `picotool load -x`).
On a board whose OTP already has secure boot enabled, only the signed `.uf2` will run.

## Signing (secure boot)

```bash
firmware/scripts/keygen.sh                    # → firmware/keys/pincer1-boot.pem (+ .pub.pem), gitignored
cmake … -DPINCER_SIGN_KEY=firmware/keys/pincer1-boot.pem && cmake --build …
picotool info -a build-signed/pincer1_consent.uf2   # "hash: verified", "signature: verified"
```

- The key is **secp256k1** (not P-256). That is the curve the RP2350 bootrom verifies; `picotool seal`
  rejects a prime256v1 key with "Signature verification failed", and the SDK's `tools/example_keys` are
  secp256k1. `keygen.sh` creates the right one.
- CMake wires the SDK functions `pico_set_otp_key_output_file`, `pico_sign_binary`, `pico_hash_binary`
  (`tools/CMakeLists.txt` of the SDK; verified in the fetched 2.3.1 sources). After link, `picotool seal
  --sign --hash` rewrites the ELF in place and writes `<build>/pincer1_consent.otp.json`:
  `bootkey0` (SHA-256 of the public key), `boot_flags1.key_valid`, `crit1.secure_boot_enable`.
- One key per product line. It never enters the repository (`.gitignore`: `firmware/keys/*`, `*.pem`,
  `*.otp.json`). Lose it and provisioned boards can never take new firmware; leak it and anyone can sign
  for them. `firmware/keys/README.md` has the rules.

### OTP provisioning (one-way, once per board)

```bash
# board in BOOTSEL mode
firmware/scripts/provision-otp.sh build-signed/pincer1_consent.otp.json \
    --scp03 scp03-keys.bin          # the 48-byte set the SE050 was rotated to (scripts/provision-se050.md)
    # --disable-debug               # production units: kills SWD forever
picotool load -x build-signed/pincer1_consent.uf2
```

The script asks you to type `BURN`. It runs `picotool otp load <otp.json>` (boot key hash + key valid +
secure boot enable), `picotool otp load -e -s 0xC0 scp03-keys.bin` (24 ECC rows, `src/secrets.h`
`SECRETS_OTP_ROW`) and sets `PAGE3_LOCK1` so page 3 is read-only for secure firmware and inaccessible
to non-secure code and to the USB bootloader. **OTP bits only go 0→1. There is no undo.**

## SCP03 (SE050 platform binding)

`src/scp03.c` implements GlobalPlatform SCP03 (INITIALIZE UPDATE, EXTERNAL AUTHENTICATE, session-key
KDF, C-MAC+C-ENC+R-MAC+R-ENC, security level 0x33) over `src/aes128.c`, and `signer_se050.c` plugs it
into the middleware's session hooks (`fp_Transform` / `fp_RawTXn` / `fp_DeCrypt`) exactly where the NXP
SSS layer plugs its own (`se05x_Transform_scp`, `nxpSCP03_Decrypt_ResponseAPDU`). The byte formats and
counter rules were taken from the submodule (`nxScp03_Com.c`, `fsl_sss_se05x_scp03.c`, `se05x_tlv.c`),
including the SE050-vs-SE051 command-counter difference (applet < 04.03.00 vs ≥, read from the SELECT
response like `sm_connect.c` does). Key version `0x0B`.

Why not the middleware's own SCP03 (`SSS_HAVE_SCP_SCP03_SSS`): it is written against the SSS host-crypto
abstraction (`sss_host_mac_*`, `sss_host_symmetric_*`, key stores, RNG contexts), which means compiling
the whole SSS layer plus its mbedtls port. pico-sdk 2.3.1 does bundle mbedtls 3.6.6 (`pico_mbedtls`), so
that path exists, but it would have pulled ~10 k lines of untestable integration into the trusted image.
Instead the protocol is ~300 lines here, host-tested against vectors from an independent Python
implementation (`cryptography`, itself checked against FIPS-197 / RFC 4493), with the AES in
`src/aes128.c` (written for Pincer1, same licence situation as `sha256.c`; not tiny-AES-c, which could
not be vendored verbatim offline). AES is table-based and not constant-time; it protects only the
in-robot link between controller and chip.

Keys: `src/secrets.c` reads 48 bytes from OTP rows `0x0C0..0x0D7` through the bootrom
(`rom_func_otp_access`, `pico/bootrom.h`, ECC mode). Blank rows → fall back to `PINCER_DEV_SCP03_KEYS`
if compiled in (`hello` then reports `"dev_keys":true`) → otherwise the SE050 is not opened at all
(`se050:false`): a bound chip refuses plain APDUs anyway, and we never want to find out whether this one
does. Rotating the chip's keys needs NXP tooling on the bench: `scripts/provision-se050.md`.

## Trusted display

`src/display.c` drives a 2.42" 128×64 monochrome OLED (SSD1309, 4-wire SPI on SPI0: GP18 SCK, GP19 MOSI,
GP17 CS, GP20 D/C, GP21 RES, 8 MHz) through a 1 KB page-addressed framebuffer and a three-function hardware
interface (`cmd` / `data` / `reset`) that `main.c` implements and the host tests mock. Fonts: a 5×7 ASCII
face (96 glyphs embedded, 6 px cells, 21 per line) and an 8×16 face derived from it at render time
(columns 1–3 doubled, rows doubled) for amounts; `·` and `…` are drawn as extra glyphs. Everything that
decides *what* is on the screen is in `src/trusted_ui.c` and is pure logic:

- **Sign requests** are rendered from the controller's own parse, never from `claims` or the daemon's
  `summary` text. Header = chain and verb (`SOLANA · SEND`, `BASE · CALL`, `BITCOIN · SEND`, `EVM · TYPED
  DATA`), amount in native units (SOL / ETH, BNB, POL, AVAX by chain id / BTC) in the large face (small and
  wrapped when it would not fit: a digit is never cut), destination as the first 6 and last 6 characters of
  the chain's encoding (base58; `0x` + EIP-55 hex; bech32 / base58check), token amounts in raw units with the
  mint / contract short form and the recipient, EVM `chainId` and `nonce` and worst-case fee, Bitcoin fee in
  BTC and sat, inputs and change. Every approval, operator grant, unknown contract / unknown program, lookup
  table, high fee, unknown output and non-ALL sighash is a boxed `! …` line. Messages show length and a
  text or hex preview; EIP-712 shows the full digest the controller computed.
- **Pages.** Content is 48 px (six small lines) above a 16 px footer. Longer requests paginate (up to 8
  pages; a request that still does not fit is refused rather than shown partially). A short touch turns
  the page (`TOUCH = next 1/3`), the final page's footer reads `TOUCH TO SIGN ·` / `hold 3s = cancel`.
  Consent-wise: the request sits in `need:"display"` until page 1 is rendered, then `pending(touch)`; a
  touch before the last page has been shown only turns pages; a long touch cancels (`denied:user`).
  `inheritance_set` arming keeps its long touch (`HOLD 3s TO APPROVE`).
- **Idle** shows `PINCER1 · READY|LOCKED|NEW DEVICE`, the active slot's three addresses in short form (so
  the decoy looks exactly like a wallet) and `! PIN NOT SET` until `set_pin`.
- **PIN entry** shows the operation being authorised (`UNLOCK`, `SET PIN`, `POLICY` with the limits or the
  router being added/removed) and one `*` per tap; the gaps, which are the secret, are never shown.
- **Denials and results** show the reason word (`DENIED` / `claims_mismatch`, `SIGNED`, `WIPED`, `ROUTER
  ADDED · EVM` with the full address wrapped) for a few seconds, then the state screen returns.
- **Seed words** after `init`: 28 pages, see below.

Every render is reported to the daemon as `{"t":"event","ev":"display","page":n,"pages":m}` so speech can
follow the page the owner is looking at.

### init and the words

The long touch on `init` derives both key sets and imports them into the SE050, then the display takes over:
real words 1–24 (four per page, `SEED WORDS 1/6` …), a check page, the real words again (`SEED CHECK 1/6`,
"compare to your copy"), a `DECOY · SECOND SEED` page, the decoy words, a check page, the decoy words again,
and a `SEED · DONE` page. A short touch advances, holding 3 s at any point (or 10 minutes without a touch,
or `lock` / `duress`) aborts: the imported keys are destroyed and the request is `denied`. Only the touch on
the last page marks the device `initialized` and sends `{"t":"init",…,"pubkey","addresses"}` to the host.
The words live in RAM for the duration of the pages and are zeroised afterwards; no command returns them
(`reveal_seed` → `denied:not_supported`). Losing the paper means losing the wallet, as it should.

## Host time and the daily limit

The daemon sends `{"t":"time","unix":…}`; the controller has no clock of its own and used to roll the daily
spend counters over on any host day change. Now (`consent_set_time`): a value lower than the last accepted
one, or more than 36 hours ahead of the controller's interpolated clock, is ignored and answered with
`{"t":"event","ev":"time_rejected"}`; the first value after boot is always accepted. The counters reset only
when the host day differs from the stored day **and** `powered_seconds` (the same uptime counter the
inheritance timer uses) has advanced by at least 20 hours since the last rollover; otherwise the stored day
is left alone and the rollover happens at the next `time` once the uptime is there. A compromised daemon can
therefore get at most one extra daily budget per 20 hours of powered uptime, which is what a real day gives it anyway.

## Router allowlist

`src/routers.c`: Jupiter v6 (`JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4`), 1inch v6
(`0x111111125421cA6dc452d289314280a0f8842A65`) and 0x Exchange Proxy
(`0xDef1C0ded9bec7F1a1670819833240f027b25EfF`) are compiled in; THORChain routers are chain-specific and
left for the owner. Up to 8 owner entries are kept in the main flash record and changed only by
`{"t":"policy","add_router"|"remove_router":{"chain":"evm"|"solana","address":…}}` after the PIN rhythm; the
display shows the full address while the rhythm is tapped and again after the change. `claims.routers` is
accepted and ignored. An EVM call into a `to` outside the list is `unknown_contract`, a Solana program that
is neither a core program (System, Token, Token-2022, Associated Token, Compute Budget, Memo) nor listed is
`unknown_program`; both must be declared by the daemon and are rendered as `! UNKNOWN CONTRACT` /
`! UNKNOWN PROGRAM` on the display. `hello.routers` lists both sets.

## PIN rhythm

4–8 taps on the head. The gap between taps is **short** below 600 ms and **long** from 600 ms; a pause of
1.5 s ends the rhythm. "short short long short" is five taps. `touch.c` classifies (pure logic, tested
at the 595/600 ms boundary and across the 32-bit tick wrap), encodes the rhythm as 8 bytes
(`'P' 'R' n gaps ~n ~gaps 5A A5`) and that code is the SE050 UserID value. Entropy is small (≤ 2^7 patterns
per length); the point is not the rhythm's secrecy but the chip's **5-attempt counter** behind it.

- `init` (long touch, then the word pages on the display) creates the keys with a policy naming the PIN
  object (id `0x50494E10`); then `set_pin` captures the rhythm twice and creates the UserID object (`Se05x_API_WriteUserID`, max attempts
  5, `INS_AUTH_OBJECT`). Until `set_pin`, nothing can sign (`pin_not_set`).
- `unlock` and `policy` need the rhythm: `CreateSession(PIN object)` + `VerifySessionUserID`; signatures
  then run inside that session; `lock` closes it. Ordinary `sign` still needs a plain touch.
- A wrong rhythm → `denied:wrong_pin`, chip counter and mirrored `pin_failures` (in flash, shown in
  `hello`) increment. The fifth → `denied:wiped`: storage zeroed, key objects and PIN object deleted
  in the chip, `event:wiped`. **The 24 words are the only recovery.**
- **Second rhythm** (`set_duress_pin`, step 3 of the init flow or any time later under the real PIN,
  replaceable, refused while the decoy slot is active): a second UserID object (id `0x50494E11`). The decoy
  keys are imported with a policy naming both objects, the real keys only the real one, so a session opened
  with the second object can sign with the decoy keys and nothing else. `unlock` runs `signer_pin_verify2`:
  one round against the second object, closed, then one round against the real object (or, when the second
  already matched, against the second again) so the real object never sees a duress attempt and every unlock
  costs the same two create/verify sequences. The second object is created without an attempt limit because
  every real unlock is one failed attempt against it by construction; the five-strike rule stays on the real
  object (chip) and on the mirror counter (any wrong rhythm). A duress match completes the `unlock` exactly
  like a real one (`consent_pin_verdict`, `CONSENT_VERDICT_DURESS`): same reply, same idle screen, counter
  untouched, no event, beacon not pulsed, decoy slot active, allowance cleared. Used for anything but
  `unlock` it is simply a wrong PIN. The two rhythms must differ (`pin_mismatch`), compared on the controller
  against the real code that was just verified (or just set). The software signer keeps a salted SHA-256 of
  it in the flash record; the chip keeps the value itself.

## Multi-chain custody (EVM + Bitcoin)

One seed, three keys per slot (real and decoy), all derived at `init` from the same 24 words and imported
into the SE050 as non-exportable key pairs with the same PIN-gated policy as the Solana key:

| chain | derivation | curve | address | SE050 object (real / decoy) |
|---|---|---|---|---|
| Solana | `m/44'/501'/0'/0'` (SLIP-0010) | ed25519 | base58 pubkey | `0x50494E01` / `02` |
| EVM | `m/44'/60'/0'/0/0` (BIP32, own CKD over `hmac_sha512.c`) | secp256k1 | keccak-256 of X‖Y, EIP-55 hex | `0x50494E03` / `04` |
| Bitcoin | `m/84'/0'/0'/0/0` (BIP84 native segwit) | secp256k1 | `bc1q…` P2WPKH (bech32) | `0x50494E05` / `06` |

Any standard wallet restores all three from the words ("abandon ×11 about" gives
`0x9858EfFD232B4033E47d90003D41EC34EcaEda94` and `bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu`, the published
test values; `test_keygen` checks exactly that). `hello` carries `addresses:{solana,evm,bitcoin}` for the
active slot, so the decoy has its own three. The public account node `m/84'/0'/0'` (xpub, not secret) is
kept in flash so that change outputs at `m/84'/0'/0'/1/i` can be recognised by re-deriving the child key.

**What the SE050 does for secp256k1.** The Plug & Trust middleware ships curve id `kSE05x_ECCurve_Secp256k1`
(0x10) and the parameter table for the SE050_E applet; `signer_se050.c` creates the curve on the applet
(`Se05x_API_CreateCurve_secp256k1`) the first time a key is imported, writes the key pair with
`WriteECKey` (public point `04‖X‖Y`, no byte reversal on this curve), reads it back and refuses to keep an
object whose public key is not the one we derived. Signing is `ECDSASign` over the 32-byte digest (the
algorithm id only names the hash length, as `sss_se05x_asymmetric_sign_digest` does); the DER answer is
decoded, normalised to low-s, and the recovery id is obtained by recovering our own public key from the
signature (`secp256k1_find_recid`), which doubles as the "never emit a bogus signature" check. **What it does
not do:** the SE050 produces ECDSA with its own random nonce, so signatures are not RFC 6979 deterministic
(the software signer's are); it does not hash (we hash on the controller); and whether this OEF variant
accepts Koblitz curves depends on the SE050 variant. If it does not, the build
fails loudly at `init` (`denied:policy`) rather than silently falling back: secp256k1 signing in software
exists only in the `PINCER_SOFT_SIGNER` build, which `hello` reports as `se050:false`.

**EVM (`sign` kind `evm_tx`)**, `src/evmtx.c`: strict canonical RLP, legacy EIP-155 (9 fields; pre-155
6-field transactions are refused, type 1 and unknown envelopes are refused) and EIP-1559 type 2. Parsed:
chain id, nonce, gas limit, max fee (or gas price), `to`, value, calldata, access list (bounded, ignored).
Recognised calldata: ERC-20 `transfer` `a9059cbb`, `approve` `095ea7b3` (2^256−1 → `approve_unlimited`,
anything else → `approve`), `transferFrom` `23b872dd` (an outflow only when `from` is the owner),
`setApprovalForAll` `a22cb465` (`true` → `set_approval_for_all`), `increaseAllowance` `39509351` (→
`approve`). Exact ABI lengths are required; anything else is a contract call that must either be to a
router in the controller's own allowlist ("Router allowlist" above) or be declared `unknown_contract` (so is
contract creation). An ERC-20 transfer's recipient must equal the `to` of a `claims.token_out` entry for that
token, not only its amount. Digest =
keccak-256 of the bytes as given; signature = `r‖s‖v`, low-s, `v` = 27+recid (legacy; the daemon applies
EIP-155) or recid (typed). `evm_msg` = EIP-191 `personal_sign` hashed on-device, or EIP-712 typed data:
the controller receives the JSON and the claimed digest, re-hashes domain + message for structs of
primitive types (`address bool uintN intN bytesN bytes string`) and refuses nested structs and arrays with
`unsupported_typed_data` rather than trusting a digest it cannot check (`src/eip712.c`). Daily limit in
wei (`u256`, so it is not capped at 18.4 ETH).

**Bitcoin (`sign` kind `btc_psbt`)**, `src/btcpsbt.c`: BIP-174 v0 PSBT (PSBTv2 refused), ≤ 8 inputs / 8
outputs, ≤ 4096 bytes. Per input: `witness_utxo`, `sighash_type`, `bip32_derivation`; per output:
`bip32_derivation`. An owner input is one whose derivation names the controller's key at
`84'/0'/0'/0/0` **and** whose `witness_utxo` pays `0014‖hash160(key)`; any other input makes the PSBT
unsignable (the controller does not co-sign into transactions it cannot account for). Change = an output
to our own address, or one whose derivation path is `84'/0'/0'/0'/1/i` (i < 100 000) and whose key and
script re-derive from the stored account xpub. Fee = inputs − outputs. BIP-143 sighash per input
(`ALL`, `SINGLE`, `ANYONECANPAY` variants; `NONE` is never signed), DER low-s partial signatures inserted
as `PSBT_IN_PARTIAL_SIG`; the daemon finalises. `btc_msg` = BIP-322 simple for P2WPKH (to_spend /
to_sign built on-device, witness returned). Daily limit in sats.

Checks against `claims` (`src/policy.c`, `policy_check_evm` / `policy_check_btc`) and the per-chain
limits, elevate and `policy` requests (`consent.c`) are listed in `docs/PROTOCOL.md` §1. A Bitcoin PSBT that
spends anything must name its destination in `claims.dest`; `unknown_change` only covers *additional* outputs. A `signMessage`
of any kind that parses as a Solana message, an EVM transaction or a PSBT is refused.

Vendored for this: micro-ecc (BSD-2, secp256k1 field/point arithmetic; ECDSA signing, RFC 6979 nonces,
low-s and public-key recovery are ours in `src/secp256k1.c` because micro-ecc's own `uECC_sign` exposes
none of them), tiny_sha3's Keccak-f (MIT; `src/keccak.c` adds the 0x01 padding Ethereum uses), the bech32
reference encoder (MIT). RIPEMD-160, HMAC-SHA256 and the 256-bit integer helpers were written here.

## Inheritance (dead-man's switch)

`inheritance_set` is a `sign` with three extra rules (`main.c` `handle_inheritance_set`): the message's
first instruction must be System `advanceNonce` on an account that is not the owner key
(`solmsg_starts_with_advance_nonce`), it must fit one SE05x APDU (`CONSENT_INH_MSG_MAX` 860 bytes) and
have ≤ 8 required signers. After the long touch (a plain touch when replacing an arrangement) the real key
signs it once; message + signature go to the **inheritance region**, the reply carries the signature only.

The timer (`consent.c`): `consent_tick()` turns the ms delta into `powered_seconds` (u64, saturating,
wrap-safe), every debounced press (`TOUCH_EV_PRESS`, so rhythm taps too) and every `consent_touch()` set
`last_touch_powered_seconds`. `consent_inheritance_release()` says yes once
`powered_seconds − last_touch ≥ inactivity_days × 86400`, regardless of lock / duress / initialised state.
Warnings fire once per `warn_days` threshold (bitmask `warned`, persisted, cleared by a touch).

Persistence: the record is written when `ih_dirty` is raised, which happens (a) every
`CONSENT_POWERED_CHECKPOINT_S` = 3600 s of uptime, (b) on arm / clear / warning, (c) on a touch when a
checkpoint is already due (so a rhythm never costs 8 flash writes), and (d) alongside every main-record
write (`persist()`), so a sign or lock also checkpoints the counter. On boot the counter resumes from the
stored value: it never goes backwards, but up to one hour of uptime before a power cut is lost, which a
thief can only use to *slow* the timer by power-cycling, not to stop it, and only as effectively as by
touching the pad. Wear: ≤ 24 checkpoint writes/day plus one per state change, alternating two sectors.

The PIN wipe writes an all-zero *main* record and deletes the chip objects; it does not write the
inheritance region at all (`do_wipe`, `consent_wipe` leaves `c->ih` alone). `inheritance_release` therefore
keeps working on a wiped, uninitialised controller: that is the point of the feature. The arrangement
records the public key that signed it; `inheritance_clear` and a replacing `inheritance_set` require
owner authority (initialised, PIN set, unlocked, no duress) **and** that the current real key equals that
signer. So after wipe + `init` (by a thief or by the owner) the old arrangement can neither be cleared nor
replaced, only released; it spends the old account, which the owner can empty with the 24 words. If both
inheritance sectors are corrupt the controller starts with an empty timer and no arrangement.

Flash layout (`main.c`): `STORAGE_BASE = PICO_FLASH_SIZE_BYTES − 8 KB` holds the 512-byte main record
(v3, unchanged), `STORAGE_INH_BASE = STORAGE_BASE − 8 KB` holds the 1280-byte inheritance record
(`"PCI1"` v1: counter 16 B, arrangement 79 B incl. heir + signer, message length + 860 B, signature 64 B,
CRC-32; 1035 bytes used). Each region alternates between its two sectors; the newest valid CRC wins.

## Field updates (A/B partitions)

The image carries a partition table (`pt/pincer1.json`: A and B, 1 MiB each, secure Arm only; the
unpartitioned tail holds the storage regions and the trial ledger). `picotool load` of a build with the
table writes it along with the image; from then on new firmware arrives over the daemon link
(`docs/PROTOCOL.md`, "update"): the daemon streams the signed `.bin` into the spare partition, the owner
holds the pad for 3 s while the display shows the version and hash, the controller reboots into it on
trial, and the daemon commits it once the new image answers `hello`. Three boots without a commit and the
new image invalidates itself; the bootrom boots the previous one. The bootrom checks the signature against
the OTP key hash on every boot, so the update path adds no trust: an image not signed with the board's key
is refused before it runs, whatever the daemon sent.

```bash
# on the workstation, after a signed build
scp build/pincer1_consent.bin pincer1.local:/tmp/
# on the robot
scripts/fw-update.sh /tmp/pincer1_consent.bin 0.5.1        # → POST /firmware/update on the daemon
curl localhost:7401/firmware                                 # fw, partition, staged, trial
```

`pico_set_binary_version` stamps the major.minor from `PROTO_FW_VERSION` into the image; the daemon
refuses a file whose stamp disagrees with the version named on the command line. `src/update.c` is the
engine (pure logic over a flash interface, tested on the host); `main.c` owns the partition discovery via
the bootrom, the ledger sector and the reboot.

## Host tests

All logic that does not need hardware is compiled with the host compiler and tested:

```bash
cmake -S firmware -B build-host -DBUILD_HOST_TESTS=ON && cmake --build build-host && ctest --test-dir build-host
```

| suite | covers |
|---|---|
| `solmsg` | legacy and v0 message parsing on hand-built byte fixtures: System transfer / createAccount / transferWithSeed / assign, SPL transfer / transferChecked / approve / setAuthority / closeAccount / burn, lamports-out and token-out sums, dangerous-instruction detection, unresolved lookup tables, `looks_like_message` |
| `policy` | the Solana checks against claims (owner is signer, lamports ≤ claimed, destination equals `claims.dest`, every token recipient covered by a `token_out` entry of its mint, programs ⊆ claimed, dangers declared, `unknown_program` for non-core programs outside the router list: built-in Jupiter passes, a host-only "router" does not, an owner-added one does, the built-in addresses re-encode to their published checksummed strings) with pass and fail cases; EVM checks (1)–(5): chain id + `to`, value, gas×fee incl. saturation, ERC-20 amounts per token and `transferFrom` source, approve / unlimited / setApprovalForAll / router vs `unknown_contract` / creation; Bitcoin checks (1)–(5): network, owner-input count, destination, sats_out, fee, `unknown_change` for undeclared outputs, sighash and high-fee flags, consolidation |
| `consent` | state machine: idle → pending(display → touch / long_touch / pin) → signed / denied; second rhythm: gated flow (real VERIFY → CONTINUE → confirm), identical-to-real and non-matching confirmations refused, init-flow variant, replaceable, refused in duress, a duress verdict on `unlock` completes as a plain unlock into the decoy slot without touching the failure counter, on any other op it is a wrong PIN, the real rhythm leaves duress, `decoy_addresses` gated and refused in duress; ttl timeout, lock / unlock (PIN), duress slot switch, elevate window, daily spend and day rollover (host day change + 20 h powered; no rollover with 2 h; the next day needs another 20 h), host time rejected when backwards or > 36 h ahead and the counters untouched, display gating (no touch counts before the screen, short touches page, the final page must have been shown, long touch cancels, non-gated unlock untouched), the 28-page seed-words flow with its 10-minute window, cancel and timeout handing the keys back for destruction, router add / remove through the PIN with a full list and lock gate, allowance; set_pin twice + mismatch, unlock/policy gated on the PIN verdict, wrong-PIN counter across reboot, 5 failures → wipe, link error does not count; per-chain limits: wei (u256) and sats counters independent of lamports, `policy` setting any subset, elevate bound to one chain and consumed only by that chain, saturation refused, rollover clears all three, lock gates |
| `keygen` | software signer second rhythm: `verify2` real / duress / wrong, a duress session signs only with the decoy key, replace, persist, wipe; BIP39 24-word mnemonic with valid checksum, SLIP-0010 ed25519 `m/44'/501'/0'/0'`, determinism, sign/verify with the derived key, seed zeroisation; software signer PIN gating and wipe; `keygen_keys_from_seed` on "abandon ×11 about" → `0x9858EfFD…da94` and `bc1qcr8te4…6fyu`, account xpub re-derives the receive key; software secp256k1 signer: import, low-s + recovery id, PIN gate, export / wipe / load |
| `storage` | double-slot flash record (v6: + per-chain limits, EVM/BTC addresses, BTC account xpub, secp256k1 soft blobs, rollover uptime, owner routers, duress flag + salted hash; 1280 bytes) with CRC, round trip, v2–v5 rejected, corruption recovery on an in-memory flash mock, wipe record, owner-router persistence round trip |
| `inheritance` | arming needs a durable-nonce message + long touch (replace = touch), parameter and duress / lock gates; `advanceNonce` detection on built messages; `days_since_touch` tenths math; release denied before and allowed at exactly `inactivity_days`, with no host time; every kind of touch resets the timer; warnings once per threshold, re-armed by a touch, not repeated after reboot; lock / duress / 5-failure wipe keep the arrangement and release still works; clear needs a long touch and removes it; uptime counter: sub-second accumulation, hourly checkpoint, resume-never-rewind across a simulated reset, future `last_touch` clamped, 32-bit tick wrap over 50 days; inheritance record round trip (max message, armed-without-tx, CRC / magic / version, cross-decoder rejection), slot alternation and torn-write recovery; `inheritance_*` parsing and replies, `hello.inheritance`, `inheritance_warning` event |
| `proto` | JSON line framing, request ids, u64-as-decimal-string encoding, `set_pin`, `set_duress_pin`, `decoy_addresses`, `hello` PIN / `display` / `routers` fields and no duress / decoy field at all, `init` reply with addresses + decoy addresses and no mnemonic, `reveal_seed`, `event:display`, `policy` router changes, `token_out.dest` / `.to`, `pending` with `confirm`; `sign` kinds `evm_tx` / `btc_psbt` / `evm_msg` (bytes or typed_data + digest) / `btc_msg` with their claims shapes and rejections, per-chain `elevate` and `policy`, `hello.addresses` and per-chain limits, `pending.parsed` for EVM and Bitcoin, 65-byte and PSBT `signed` replies, 4096-byte payload bound |
| `touch` | debounce, short vs long (≥ 3000 ms); rhythm capture, gap boundary, end pause, 8-tap cut-off, too-few, encoding, string form |
| `scp03` | AES-128 (FIPS-197), AES-CMAC (RFC 4493), SCP03 session keys / cryptograms / EXTERNAL AUTHENTICATE, wrap and unwrap against the Python trace, R-MAC and padding rejection, both counter modes |
| `secrets` | 96-hex-char key parsing (good/bad), dev fallback source, zeroed output when no keys |
| `util` | base58, hex, constant-time compare |
| `secp256k1` | Keccak-256 / RIPEMD-160 / HMAC-SHA256 (RFC 4231) / SHA-256d reference digests; `u256` decimal/hex/multiply/saturation; BIP-32 test vector 1 (master, m/0H, m/0H/1, m/0H/1/2H keys + chain codes, public derivation = xpub); BIP-84 and BIP-44 addresses of the standard mnemonic; EIP-155 example: exact RFC 6979 r/s, v, recovery, tampering, high-s normalisation; BIP-143 key, pubkey, DER decode/encode of the published signature; EIP-55 addresses; BIP-173 / BIP-350 / base58check address round trips |
| `evmtx` | EIP-155 example bytes + signing hash; legacy ERC-20 transfer, type 2 transfer and unlimited approve with access list (digests from an independent Python RLP encoder + pycryptodome keccak); `transferFrom` / `setApprovalForAll` / `increaseAllowance` / truncated calldata / unknown call / creation; rejections: pre-155, type 1, trailing byte, truncated, non-canonical RLP, leading zeros, chain id 0, r/s placeholders, 19-byte `to`, 65-bit nonce, priority > max fee, 8-field type 2; EIP-191 digests; EIP-712: published "Ether Mail" domain separator, ERC-2612 permit digest, every primitive type incl. negative int, refusals (nested struct, array, unknown / unfitting types, bad address checksum, primaryType = domain) |
| `display` | SSD1309 init sequence (off first, on last, 64-line mux, horizontal addressing, no SSD1306 charge pump) and flush (column/page window + 1024 bytes, only when dirty) over a mock bus; NULL hardware = no traffic; pixel / line / rect primitives with clipping; 5×7 glyph table spot checks and rendering (normal, inverted cell, 21 cells per line, right-edge clipping, `·` and `…`, non-ASCII → `?`), 8×16 derivation |
| `trusted_ui` | amount formatting (9 / 8 / 18 decimals, u256, zero trimming), short address forms for base58 / EIP-55 / bech32 and non-standard scripts, chain names and units, pagination by pixel height, 21-cell cut with ellipsis vs wrap-never-cut for amounts, truncation flag; Solana send / token / burn / message / inheritance screens, EVM send / call / approve / approve-unlimited / transfer / create / typed-data screens, Bitcoin send / consolidate / message screens with every boxed warning; idle, PIN dots, notices, prompts, router page with the full address; the 28 word pages (each set exactly twice, never mixed); rendering of header bar, big text, boxes and the four footers compared pixel-for-pixel |
| `btcpsbt` | hand-built BIP-174 fixtures for the standard mnemonic: parse, owner inputs, change by path via the account xpub (and the three ways it fails), paying ourselves, far change index; refusals: foreign input, foreign script, unheld path, no derivation, no witness_utxo, outputs > inputs, SIGHASH_NONE, SINGLE beyond outputs, bad magic, truncation, trailing bytes, PSBTv2, scriptSig present, 9 inputs, finalised input; flags: SINGLE / ANYONECANPAY, high fee at the 10 % boundary, consolidation; BIP-143 "Native P2WPKH" sighash vector and the published signature; partial-signature insertion re-parses with the expected bytes; BIP-322: message hash, to_spend / to_sign txids and the published deterministic simple signatures for "" and "Hello World" |



## Layout

```
src/main.c           USB CDC loop, dispatch, 5 ms tick, LED, rhythm capture, wipe, SPI display glue, screens per state
src/proto.c          framing + message (de)serialisation (cJSON)
src/consent.c        state machine, display gating, lock/duress/elevate/allowance, PIN sequencing + failure
                     mirror, host-time rules, router policy, powered-uptime timer + inheritance      (pure logic)
src/display.c        SSD1309 framebuffer driver, 5x7 + derived 8x16 fonts, mockable bus              (pure logic + SPI port)
src/trusted_ui.c     what the display shows: sign documents from the parse, idle / PIN / words / notices (pure logic)
src/routers.c        swap-router allowlist: built-ins + owner entries                                (pure logic)
src/solmsg.c         Solana message parser                                             (pure logic)
src/policy.c         claims vs parse checks, Solana / EVM / Bitcoin                   (pure logic)
src/keygen.c         BIP39 + SLIP-0010 + BIP32 paths for all three chains             (pure logic)
src/evmtx.c          RLP, legacy + EIP-1559 parser, ERC-20 calldata, EIP-191         (pure logic)
src/eip712.c         EIP-712 typed-data hashing, primitive structs only               (pure logic)
src/btcpsbt.c        BIP-174 PSBT, BIP-143 sighash, partial sigs, BIP-322            (pure logic)
src/secp256k1.c      ECDSA / RFC 6979 / low-s / recovery / DER over micro-ecc        (pure logic)
src/bip32.c          BIP32 CKDpriv / CKDpub                                           (pure logic)
src/chainaddr.c      EVM EIP-55 and Bitcoin bech32 / base58check addresses           (pure logic)
src/u256.c, keccak.c, ripemd160.c   256-bit integers, Keccak-256, RIPEMD-160          (pure logic)
src/storage.c        flash persistence: main record + inheritance record, double slot + CRC  (pure logic + flash port)
src/update.c         field updates: staged image in the spare A/B partition, hash + image checks, trial ledger  (pure logic + flash port)
pt/pincer1.json      partition table embedded in the image (A/B 1 MiB, secure Arm; storage + ledger in the unpartitioned tail)
src/touch.c          debounce / long-touch / PIN tap rhythm                            (pure logic)
src/scp03.c          GlobalPlatform SCP03 secure channel                               (pure logic)
src/aes128.c         AES-128 ECB/CBC/CMAC                                              (pure logic)
src/secrets.c        SCP03 keys from RP2350 OTP (bootrom), dev fallback                (pure logic + OTP)
src/rng.c            hardware RNG (pico_rand, TRNG-fed on RP2350) + ADC jitter mixed into keygen
src/signer.h         two-slot signer interface (0 real, 1 decoy), ed25519 + secp256k1 x2, PIN + second rhythm, wipe
src/signer_se050.c   SE050 via NXP Plug & Trust (APDU over T1oI2C), SCP03 channel, UserID session, policies
src/se050_port.c     RP2350/RP2040 glue for the middleware: I2C0, delays
src/signer_soft.c    software ed25519 fallback (orlp/ed25519), hashed PIN
src/compat/          fsl_debug_console.h shim so the middleware builds outside the NXP SDK
scripts/             keygen.sh, provision-otp.sh, provision-se050.md
keys/                signing key (gitignored), README
third_party/         cjson (MIT), ed25519 (zlib), bip39 wordlist, plug-and-trust (BSD-3, submodule),
                     micro-ecc (BSD-2), tiny_sha3 (MIT), bech32 reference (MIT)
test/                host unit tests, assert-based, no framework
```

## Security properties enforced here (see `docs/SECURITY.md`)

- No signature without a touch on GP15; the pin is read only by this firmware.
- No touch counts before the trusted display has shown the request, rendered from the controller's own
  parse; the final page must have been shown; a long touch cancels. Host-supplied text never reaches the panel.
- The message is parsed on-device; `lamports_out`, destination (and every token recipient), program set
  and dangerous instructions must match the daemon's `claims` or the request is denied.
- Programs / contracts outside the controller's own router allowlist are `unknown_program` /
  `unknown_contract`: declared or denied, and always shown as DANGER. The host cannot extend the list;
  the owner can, with the PIN, and sees the address on the display.
- The daily counters reset only on a host day change backed by 20 hours of the controller's own powered
  uptime; backwards or far-forward host time is ignored.
- `signMessage` payloads that parse as a transaction of any chain (Solana message, EVM RLP, PSBT) are refused.
- EVM: chain id, `to`, value, worst-case fee, ERC-20 amounts and every approval / operator grant / unknown
  contract call must match `claims`; EIP-712 digests are recomputed from the JSON or the request is refused.
- Bitcoin: only the owner's own P2WPKH inputs are signed, every non-change output must be the declared
  destination, change is recognised by re-deriving the key from the account xpub, fee and sighash must be
  declared, `SIGHASH_NONE` is never signed.
- Daily limits are kept per chain (lamports, wei, sats), an elevate raises one chain for one request.
- Daily limit, elevation, allowance, lock, duress are enforced and persisted here, not on the Pi.
- The inheritance timer is powered uptime counted here; the daemon cannot advance it, and neither
  `lock`, `duress` nor the PIN wipe can cancel a stored inheritance (only a long touch via `inheritance_clear`).
- Unlock after a lock, and policy changes, need the PIN rhythm verified inside the SE050; five failures wipe.
- A second rhythm unlocks into the decoy wallet with a reply, screen and counters identical to a real unlock;
  nothing on the USB link reports which wallet is active.
- Only this firmware (signed, secure boot) can read the SCP03 keys from OTP; only a holder of those keys
  can talk to the SE050.
- Secrets never cross the USB link. The mnemonic is shown on the trusted display at `init` and nowhere
  else; `reveal_seed` does not exist as a feature and is refused.

