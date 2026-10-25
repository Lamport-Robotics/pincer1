#!/usr/bin/env bash
# Pincer1 - burn the RP2350 OTP of ONE Pico 2: secure-boot key hash (+ enable), optional SCP03 keys,
# optional debug disable.
#
#   firmware/scripts/provision-otp.sh <build>/pincer1_consent.otp.json [--scp03 scp03-keys.bin]
#                                     [--disable-debug] [--disable-picoboot] [--yes]
#
# <build>/pincer1_consent.otp.json is written by the signed build (cmake -DPINCER_SIGN_KEY=...). It holds
#   bootkey0            SHA-256 of the signing public key (rows 0x080..0x08F)
#   boot_flags1.key_valid  bit 0 (row 0x04B)
#   crit1.secure_boot_enable (row 0x040)
# scp03-keys.bin is the 48-byte ENC||MAC||DEK set the SE050 was rotated to (scripts/provision-se050.md);
# it lands in rows 0x0C0..0x0D7 (ECC, 2 bytes per row; src/secrets.h SECRETS_OTP_ROW) and page 3 is then
# locked: secure firmware read-only, non-secure and bootloader/USB no access.
#
# ############################################################################################
# #  THIS IS ONE-WAY. OTP bits only go from 0 to 1. After this script:                       #
# #   - the board boots ONLY firmware signed with the matching private key, forever          #
# #   - the SCP03 rows cannot be rewritten; lose the key file and the chip pair is scrap      #
# #   - with --disable-debug the SWD port is dead, forever                                   #
# #  Run it ONCE per board, with the board in BOOTSEL mode and nothing else plugged in.      #
# ############################################################################################
#
# Not yet run on real hardware (no Pico 2 on the bench at the time of writing): the picotool invocations
# below follow `picotool help otp load/set` of picotool 2.3.1 and the RP2350 datasheet OTP chapter.
set -euo pipefail

otp_json=""
scp03_bin=""
disable_debug=0
disable_picoboot=0
yes=0
while [ $# -gt 0 ]; do
    case "$1" in
        --scp03) scp03_bin="$2"; shift 2 ;;
        --disable-debug) disable_debug=1; shift ;;
        --disable-picoboot) disable_picoboot=1; shift ;;
        --yes) yes=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) if [ -z "$otp_json" ]; then otp_json="$1"; shift; else echo "unexpected argument: $1" >&2; exit 2; fi ;;
    esac
done
[ -n "$otp_json" ] || { echo "usage: $0 <build>/pincer1_consent.otp.json [--scp03 keys.bin] [--disable-debug] [--yes]" >&2; exit 2; }
[ -f "$otp_json" ] || { echo "not found: $otp_json" >&2; exit 2; }
grep -q '"bootkey0"' "$otp_json" || { echo "$otp_json has no bootkey0: build with -DPINCER_SIGN_KEY first" >&2; exit 2; }
grep -q '"secure_boot_enable"' "$otp_json" || { echo "$otp_json does not enable secure boot; refusing" >&2; exit 2; }
if [ -n "$scp03_bin" ]; then
    [ -f "$scp03_bin" ] || { echo "not found: $scp03_bin" >&2; exit 2; }
    sz=$(wc -c < "$scp03_bin" | tr -d ' ')
    [ "$sz" = "48" ] || { echo "$scp03_bin must be exactly 48 bytes (ENC||MAC||DEK), is $sz" >&2; exit 2; }
fi
command -v picotool >/dev/null || { echo "picotool not found (needs 2.x)" >&2; exit 2; }

echo "== device"
picotool info -d 2>&1 | head -12 || { echo "no RP2350 in BOOTSEL mode found" >&2; exit 1; }
picotool info -d 2>&1 | grep -q 'RP2350' || { echo "the connected device is not an RP2350; aborting" >&2; exit 1; }

echo
echo "== will burn (irreversible)"
echo "  $otp_json  -> bootkey0 hash, boot_flags1.key_valid, crit1.secure_boot_enable"
[ -n "$scp03_bin" ] && echo "  $scp03_bin -> rows 0x0C0..0x0D7 (ECC) + PAGE3_LOCK1 = S:ro NS:none BL:none"
[ "$disable_debug" = 1 ] && echo "  crit1.debug_disable = 1 (SWD dead forever)"
[ "$disable_picoboot" = 1 ] && echo "  boot_flags0.disable_bootsel_usb_picoboot_ifc = 1 (no USB flash access in BOOTSEL, forever; updates come through the daemon only)"
echo
if [ "$yes" != 1 ]; then
    echo "Type BURN to continue:"
    read -r answer
    [ "$answer" = "BURN" ] || { echo "aborted"; exit 1; }
fi

echo "== 1/3 secure boot rows"
picotool otp load "$otp_json"

if [ -n "$scp03_bin" ]; then
    echo "== 2/3 SCP03 keys -> rows 0x0C0.. (ECC)"
    picotool otp load -e -s 0xC0 "$scp03_bin"
    # PAGE3_LOCK1 (row 0xF87): LOCK_BL[5:4]=3 inaccessible, LOCK_NS[3:2]=3 inaccessible, LOCK_S[1:0]=1 read-only
    # -> 0x3D, stored three times in the 24-bit row (otp_data.h PAGE3_LOCK1 R1/R2 copies).
    picotool otp set -r 0xF87 0x3D3D3D
else
    echo "== 2/3 (no SCP03 keys requested)"
fi

if [ "$disable_debug" = 1 ]; then
    echo "== 3/3 debug disable"
    tmp=$(mktemp)
    printf '{\n  "crit1": { "debug_disable": 1 }\n}\n' > "$tmp"
    picotool otp load "$tmp"
    rm -f "$tmp"
else
    echo "== 3/3 (debug left enabled; pass --disable-debug on production units)"
fi

echo
echo "done. Flash the SIGNED image now: picotool load -x <build>/pincer1_consent.uf2"
echo "The board will refuse any image not signed with the key behind $otp_json."

if [ "$disable_picoboot" = 1 ]; then
  # BOOTSEL stays (recovery through the UF2 drive is also off: the bootrom then only runs signed images
  # already in flash). Storage, the trial ledger and the partition table can no longer be read or rewritten
  # over USB. Check the exact selector on your picotool: `picotool otp list | grep -i picoboot`.
  picotool otp set BOOT_FLAGS0.DISABLE_BOOTSEL_USB_PICOBOOT_IFC 1
fi
