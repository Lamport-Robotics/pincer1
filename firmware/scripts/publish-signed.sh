#!/usr/bin/env bash
# Put the signed consent-controller image and its hashes on the GitHub release for a tag.
#
#   firmware/scripts/publish-signed.sh <tag> <signed build dir>
#
# The release workflow builds and publishes the unsigned images (reproducible, SHA256SUMS). The signing key
# stays offline, so the signed image, the one provisioned boards actually run, is built on the workstation
# (cmake -DPINCER_SIGN_KEY=…) and uploaded with this script:
#
#   pincer1-consent-<tag>-pico2-signed.uf2 / .bin   the image, signed and hashed by picotool seal
#   SHA256SUMS.signed                                their SHA-256 plus the hash of the signing public key
#                                                    (the bootkey0 value every provisioned board carries in OTP)
#
# An owner checks a file with firmware/scripts/verify-image.sh and the board with its --board flag.
set -euo pipefail
tag=${1:?tag, e.g. v0.2.0}; dir=${2:?signed build dir}
repo=${PINCER_REPO:-Lamport-Robotics/pincer1}
uf2=$dir/pincer1_consent.uf2; bin=$dir/pincer1_consent.bin; otp=$dir/pincer1_consent.otp.json
for f in "$uf2" "$bin" "$otp"; do [ -f "$f" ] || { echo "not found: $f (build with -DPINCER_SIGN_KEY first)" >&2; exit 2; }; done
command -v picotool >/dev/null || { echo "picotool 2.x is needed" >&2; exit 2; }
command -v gh >/dev/null || { echo "gh is needed" >&2; exit 2; }
picotool info -a "$uf2" | grep -qiE 'signature: *verified' || { echo "$uf2 is not a signed image" >&2; exit 1; }
picotool info -a "$uf2" | grep -qiE 'build date' && { echo "$uf2 carries a build date: rebuild from current sources" >&2; exit 1; }
key=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["bootkey0"])' "$otp" 2>/dev/null || true)
[ -n "$key" ] || { echo "$otp has no bootkey0" >&2; exit 1; }
gh release view "$tag" --repo "$repo" >/dev/null || { echo "no release $tag on $repo (push the tag first; the workflow creates it)" >&2; exit 1; }

out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
cp "$uf2" "$out/pincer1-consent-$tag-pico2-signed.uf2"
cp "$bin" "$out/pincer1-consent-$tag-pico2-signed.bin"
(cd "$out" && shasum -a 256 pincer1-consent-*-signed.* > SHA256SUMS.signed && printf '%s  signing-key (sha256 of the public key, OTP bootkey0)\n' "$key" >> SHA256SUMS.signed)
cat "$out/SHA256SUMS.signed"
gh release upload "$tag" "$out"/* --repo "$repo" --clobber
echo "uploaded to https://github.com/$repo/releases/tag/$tag"
