#!/usr/bin/env bash
# Exercises make-release → pincer-update → health → rollback in a scratch root, with systemctl stubbed.
# Runs on any machine with openssl, python3, tar, pnpm (the daemon build). deploy/jetson/update/test.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(mktemp -d); trap 'rm -rf "$root"' EXIT
export PINCER_ROOT="$root/opt" PINCER_ETC="$root/etc" PINCER_SYSTEMCTL=true PINCER_SKIP_DEPS=1 PINCER_HEALTH_TIMEOUT=4
mkdir -p "$PINCER_ROOT" "$PINCER_ETC"
"$here/release-key.sh" "$root/key.pem" >/dev/null
cp "$root/key.pub" "$PINCER_ETC/release.pub"
openssl genpkey -algorithm ed25519 -out "$root/other.pem" 2>/dev/null
fail() { echo "FAIL: $*" >&2; exit 1; }

r1=$("$here/make-release.sh" 0.0.1 "$root/key.pem" "$root")
r2=$("$here/make-release.sh" 0.0.2 "$root/key.pem" "$root")
rbad=$("$here/make-release.sh" 0.0.3 "$root/other.pem" "$root")

# unsigned-by-us is refused before anything is installed
if PINCER_HEALTH=true "$here/pincer-update.sh" "$rbad" 2>/dev/null; then fail "foreign signature accepted"; fi
[ ! -e "$PINCER_ROOT/releases/0.0.3" ] || fail "refused release left on disk"

# a tampered file inside a signed release is refused
mkdir -p "$root/t" && tar -C "$root/t" -xzf "$r1" && echo x >> "$root/t/VERSION" && tar -C "$root/t" -czf "$root/tampered.tar.gz" .
if PINCER_HEALTH=true "$here/pincer-update.sh" "$root/tampered.tar.gz" 2>/dev/null; then fail "tampered release accepted"; fi

# healthy install becomes current
PINCER_HEALTH=true "$here/pincer-update.sh" "$r1" 2>/dev/null || fail "first install"
[ "$(readlink "$PINCER_ROOT/current")" = "$PINCER_ROOT/releases/0.0.1" ] || fail "current not 0.0.1"
[ "$(cat "$PINCER_ROOT/current/VERSION")" = "0.0.1" ] || fail "VERSION"

# an unhealthy release is rolled back automatically
if PINCER_HEALTH=false "$here/pincer-update.sh" "$r2" 2>/dev/null; then fail "unhealthy release reported success"; fi
[ "$(readlink "$PINCER_ROOT/current")" = "$PINCER_ROOT/releases/0.0.1" ] || fail "no automatic rollback"

# the same release can be re-tried after removing it; manual rollback goes both ways
rm -rf "$PINCER_ROOT/releases/0.0.2"
PINCER_HEALTH=true "$here/pincer-update.sh" "$r2" 2>/dev/null || fail "second install"
[ "$(readlink "$PINCER_ROOT/current")" = "$PINCER_ROOT/releases/0.0.2" ] || fail "current not 0.0.2"
"$here/pincer-rollback.sh" 2>/dev/null || fail "manual rollback"
[ "$(readlink "$PINCER_ROOT/current")" = "$PINCER_ROOT/releases/0.0.1" ] || fail "rollback target"
"$here/pincer-rollback.sh" 0.0.2 2>/dev/null || fail "named rollback"
[ "$(readlink "$PINCER_ROOT/current")" = "$PINCER_ROOT/releases/0.0.2" ] || fail "named rollback target"
echo "ok: sign, verify, tamper-refuse, install, auto-rollback, manual rollback"
