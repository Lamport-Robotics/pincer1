#!/usr/bin/env bash
# Pincer1 - generate the RP2350 secure-boot signing key.
#
# CURVE: secp256k1, NOT P-256/secp256r1. The RP2350 bootrom verifies ECDSA over secp256k1 (SHA-256);
# picotool seal refuses a prime256v1 key with "Signature verification failed" and the SDK's own
# tools/example_keys/private.pem is secp256k1. (The project brief said P-256; that would not boot.)
#
#   firmware/scripts/keygen.sh [name]        default name: pincer1-boot
#
# Writes firmware/keys/<name>.pem (PRIVATE, gitignored) and firmware/keys/<name>.pub.pem (public).
# The private key signs every firmware image (cmake -DPINCER_SIGN_KEY=firmware/keys/<name>.pem); its
# SHA-256 hash is burned into the board's OTP once (scripts/provision-otp.sh). Lose the key and no new
# firmware can ever be loaded on a provisioned board. Leak it and anyone can sign firmware for it.
# Keep it offline (hardware token or encrypted volume), never in the repository.
set -euo pipefail
name="${1:-pincer1-boot}"
dir="$(cd "$(dirname "$0")/.." && pwd)/keys"
mkdir -p "$dir"
priv="$dir/$name.pem"
pub="$dir/$name.pub.pem"
if [ -e "$priv" ]; then
    echo "refusing to overwrite existing key: $priv" >&2
    exit 1
fi
umask 077
openssl ecparam -name secp256k1 -genkey -noout -out "$priv"
openssl ec -in "$priv" -pubout -out "$pub" 2>/dev/null
chmod 600 "$priv"
echo "private key : $priv  (600, gitignored; back it up offline NOW)"
echo "public key  : $pub"
echo "fingerprint : $(openssl ec -in "$priv" -pubout -outform DER 2>/dev/null | openssl dgst -sha256 | awk '{print $2}')"
echo
echo "build a signed image:"
echo "  cmake -S firmware -B <build> -DPICO_BOARD=pico2 -DPICO_TOOLCHAIN_PATH=... -DPINCER_SIGN_KEY=$priv"
