#!/usr/bin/env bash
# Push a signed controller image through the running daemon (docs/PROTOCOL.md §1 "update").
#   scripts/fw-update.sh build/pincer1_consent.bin 0.5.0 [http://127.0.0.1:7401]
# The daemon stages the image in the spare partition; the robot's display asks for a 3 s hold; the
# controller reboots into the new image on trial and the daemon confirms it once it answers.
set -euo pipefail
img=${1:?image .bin}; ver=${2:?version major.minor.patch}; api=${3:-http://127.0.0.1:7401}
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
printf '{"version":"%s","image":"' "$ver" > "$tmp"; base64 < "$img" | tr -d '\n' >> "$tmp"; printf '"}' >> "$tmp"
echo "checking image"
curl -fsS -X POST "$api/firmware/check" -H 'content-type: application/json' --data-binary @"$tmp"; echo
echo "staging $(stat -f%z "$img" 2>/dev/null || stat -c%s "$img") bytes; hold the touch pad for 3 s when the display asks"
curl -fsS -X POST "$api/firmware/update" -H 'content-type: application/json' --data-binary @"$tmp"; echo
