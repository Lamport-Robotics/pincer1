# firmware/keys/ — signing key (NOT in git)

This directory holds the EC **secp256k1** private key (the curve the RP2350 bootrom verifies) that signs Pincer1 firmware for the RP2350's secure boot.
Everything here except this README is ignored by git (`*.pem`, `*.key`, `*.bin`, `*.json`).

- `scripts/keygen.sh` creates `pincer1-boot.pem` (private) and `pincer1-boot.pub.pem` (public).
- `cmake -DPINCER_SIGN_KEY=firmware/keys/pincer1-boot.pem` signs the image and writes
  `<build>/pincer1_consent.otp.json` with the public-key hash.
- `scripts/provision-otp.sh` burns that hash into a board's OTP and enables secure boot. **One-way.**

Rules: the private key never leaves an offline medium except to sign a release build; one key per
product line, not per board (the hash is in every board's OTP forever); the public key is safe to share.
If a key is ever compromised, boards already provisioned cannot be rekeyed — they must be replaced.
