# Binding an SE050 to its controller: rotating the Platform SCP03 keys

**Bench procedure. Needs the SE050 on the bench, the NXP tooling, and the keys you intend to burn into
the controller's OTP. Nothing in this file has been run against a chip yet (no SE050 on the bench at the
time of writing); it is assembled from the NXP Plug & Trust middleware (v04.08.02, submodule
`firmware/third_party/plug-and-trust`) and NXP AN12436 / AN12413.**

## Why

The SE050 ships with a Platform SCP03 key set (key version `0x0B`, three AES-128 keys ENC/MAC/DEK)
whose default values are public (`sss/ex/inc/ex_sss_tp_scp03_keys.h` in the submodule, per
OEF variant). Anyone who knows them can talk to the chip. Pincer1 replaces them with keys that only
this controller knows (RP2350 OTP), so a desoldered SE050 or a swapped controller cannot open a channel,
and every APDU between the two is encrypted and MACed (C-ENC/C-MAC/R-ENC/R-MAC).

The firmware reads the keys from OTP rows `0x0C0..0x0D7` (`src/secrets.c`); the same 48 bytes must be
loaded into the chip. Order of operations matters: **rotate the chip first, then burn the OTP, then flash
the signed firmware**. A chip rotated to keys the controller does not have is unreachable (recoverable
only if you still have the keys; see "Recovery").

## 1. Generate the key set (offline machine)

```bash
openssl rand -hex 48 > scp03-keys.hex            # ENC(16) || MAC(16) || DEK(16), 96 hex chars
xxd -r -p scp03-keys.hex > scp03-keys.bin        # for picotool otp load (scripts/provision-otp.sh)
```

Keep `scp03-keys.hex` with the signing key, offline. One key set per board (unlike the signing key).

## 2. Rotate the chip (NXP tooling, chip on the bench)

Use NXP's `ssscli` (Plug & Trust middleware, host build with OpenSSL) connected to the SE050 through
any supported host (an MCUXpresso dev board, a Raspberry Pi over I2C with the `t1oi2c` connection
string, or the Pincer1 controller itself running the NXP VCOM/JRCP bridge). Authenticate with the
**current** platform keys (the defaults from `ex_sss_tp_scp03_keys.h` for a new chip), then PUT KEY the
new set under key version `0x0B`:

```bash
# connect with the current (default) platform SCP03 keys
ssscli connect se05x t1oi2c none --auth PlatformSCP \
    --scpkey_enc <default ENC hex> --scpkey_mac <default MAC hex> --scpkey_dek <default DEK hex>

# rotate: ENC MAC DEK, key version 0x0B
ssscli se05x rotate-platfscp03 0x0B <ENC hex> <MAC hex> <DEK hex>

ssscli disconnect
```

Equivalent middleware example: `sss/ex/src/ex_sss_scp03_rotate_keys` (builds with
`-DSSS_HAVE_SCP_SCP03_SSS=ON -DSSS_HAVE_SE05X_AUTH_PLATFSCP03=ON`); it performs the GlobalPlatform
PUT KEY with the DEK-wrapped new keys and the key-check values. Whichever tool you use, verify the
rotation by reconnecting with the **new** keys before you touch the controller.

Notes
- The ssscli subcommand name and argument order above are from the middleware's documentation
  (`doc/ssscli`); check `ssscli se05x --help` on your installed version before relying on them.
- SE050 variants differ in which key set is the "platform" one and in whether a key set can be
  rotated at all; the OM-SE050ARD development kit can. Check AN12436 for your OEF.
- The chip also has a "mandate SCP" switch (`Se05x_API_SetPlatformSCPRequest`): with SCP03 mandated,
  plain APDUs are refused outright. Pincer1 does not set it, because the firmware already refuses to
  run without SCP03 keys; set it on a production unit if you want the chip side to enforce it too. It is
  reversible only with the platform keys.

## 3. Burn the OTP and flash

`scripts/provision-otp.sh --scp03 scp03-keys.bin ...` writes the 48 bytes into rows `0x0C0..0x0D7`
(ECC mode) and locks page 3 against non-secure reads, together with the secure-boot rows. See that
script for the warning text; it is one-way.

## 4. Verify

Flash the signed firmware, open the USB CDC link and send `{"t":"hello"}`. You want
`"se050":true` and **no** `"dev_keys":true`. `se050:false` means the SCP03 handshake failed: wrong
keys in OTP, chip not rotated, or the applet version quirk (see `src/scp03.h`, counter mode).

## Recovery

- Keys lost after rotation: the chip is a brick for us. Replace it (the controller's OTP rows cannot be
  rewritten either, so the controller goes with it).
- Keys known: rotate again with the same tool.

## Bench-only shortcut

For bring-up with an unrotated chip, build with
`-DPINCER_DEV_SCP03_KEYS=<default ENC||MAC||DEK hex>` (the defaults from `ex_sss_tp_scp03_keys.h`).
The firmware uses them only while the OTP rows are blank and `hello` reports `"dev_keys":true`; the
daemon must refuse production mode on that flag.
