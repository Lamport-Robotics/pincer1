Built by the release workflow from the tagged commit.

- `pincer1-consent-<tag>-pico2.uf2`: the consent controller for a Pico 2 with the SE050 and the OLED.
  Unsigned: the release signing key stays offline, boards shipped with secure boot enabled get images
  signed with it. Fine for a bench board whose OTP has not been touched (`docs/BENCH.md`).
- `pincer1-consent-<tag>-pico2-softsigner.uf2`: the same firmware with a software ed25519 signer, for a
  Pico 2 without the chip. `hello` reports `se050:false`; the daemon refuses production mode on it.
- `pincer1-extension-<tag>.zip`: the browser extension, unpacked build. Load it from chrome://extensions.
- `SHA256SUMS`.

The daemon and the robot service run from source (`daemon/README.md`, `robot/README.md`).
