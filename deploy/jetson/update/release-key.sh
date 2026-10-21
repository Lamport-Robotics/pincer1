#!/usr/bin/env bash
# One-time: make the release signing key pair (ed25519, OpenSSL). The private key stays on the
# workstation that cuts releases (or offline with the firmware key); the public half goes to every robot
# as /etc/pincer1/release.pub (deploy/jetson/update/pincer-update.sh refuses unsigned releases).
#   deploy/jetson/update/release-key.sh ~/.pincer1/release-key.pem
set -euo pipefail
out=${1:?private key path}
[ -e "$out" ] && { echo "refusing to overwrite $out" >&2; exit 1; }
umask 077
openssl genpkey -algorithm ed25519 -out "$out"
openssl pkey -in "$out" -pubout -out "${out%.pem}.pub"
echo "private: $out   public: ${out%.pem}.pub  (install the public key: sudo install -m 644 ${out%.pem}.pub /etc/pincer1/release.pub)"
