#!/usr/bin/env bash
# Check a Pincer1 consent-controller image against the release page.
#
#   firmware/scripts/verify-image.sh <image.uf2> [SHA256SUMS] [--board]
#
# Prints the file's SHA-256 and what picotool reads out of it (version, image hash, signature). With a
# SHA256SUMS file from the release next to it, says whether the hash is listed there. With --board, asks
# picotool to compare the flash of the plugged-in controller with the file (the board reboots into
# BOOTSEL for it; a board provisioned with --disable-picoboot cannot be read this way and only ever runs
# what its OTP key hash accepts).
#
# Unsigned release images are reproducible: build the tag with the commands in firmware/README.md and the
# hash here must equal the one in SHA256SUMS. Signed images (the ones provisioned boards run) are listed in
# SHA256SUMS.signed on the same release, next to the hash of the key that signed them.
set -euo pipefail
img=""; sums=""; board=0
for a in "$@"; do
    case "$a" in
        --board) board=1 ;;
        -h|--help) sed -n 2,15p "$0"; exit 0 ;;
        *) if [ -z "$img" ]; then img=$a; elif [ -z "$sums" ]; then sums=$a; else echo "unexpected: $a" >&2; exit 2; fi ;;
    esac
done
[ -n "$img" ] || { echo "usage: $0 <image.uf2> [SHA256SUMS] [--board]" >&2; exit 2; }
[ -f "$img" ] || { echo "not found: $img" >&2; exit 2; }
command -v picotool >/dev/null || { echo "picotool 2.x is needed (brew install picotool / the pico-sdk tools)" >&2; exit 2; }

hash=$(shasum -a 256 "$img" | awk '{print $1}')
echo "file      : $img"
echo "sha256    : $hash"
info=$(picotool info -a "$img" 2>&1 || true)
ver=$(printf '%s\n' "$info" | awk -F': *' '/^ version:/{print $2; exit}')
echo "version   : ${ver:-?}"
printf '%s\n' "$info" | grep -qiE 'signature: *verified' && echo "signature : verified (secure-boot image)" || echo "signature : none (unsigned image; runs only on a board without secure boot)"
printf '%s\n' "$info" | grep -qiE 'build date' && echo "note      : this image carries a build date; it predates the reproducible builds" || true

if [ -n "$sums" ]; then
    [ -f "$sums" ] || { echo "not found: $sums" >&2; exit 2; }
    if grep -q "^$hash " "$sums"; then
        echo "release   : listed in $sums as $(grep "^$hash " "$sums" | awk '{print $2}')"
    else
        echo "release   : NOT in $sums"; exit 1
    fi
fi

if [ "$board" = 1 ]; then
    echo "board     : comparing flash with the file (the controller reboots into BOOTSEL)"
    picotool verify "$img" -f
fi
