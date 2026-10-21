#!/usr/bin/env bash
# Cut a signed release of the Jetson software from this checkout:
#   deploy/jetson/update/make-release.sh <version> <release-key.pem|-> [out dir]
# Produces pincer1-<version>.tar.gz holding daemon/dist + package.json + pnpm-lock.yaml, robot/ (sources,
# no venv), deploy/ (the update and rollback scripts), manifest.json (sha256 of every file) and
# manifest.sig (ed25519 over the manifest). A key of "-" leaves the release unsigned: only for a build
# installed from a checkout on the robot itself (PINCER_ALLOW_UNSIGNED=1), never for anything shipped.
# The robot installs it with pincer-update.sh; nothing in the tarball is executed before the signature
# and every hash have been checked.
set -euo pipefail
ver=${1:?version}; key=${2:?release-key.pem}; out=${3:-.}
[[ $ver =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$ ]] || { echo "version must look like 1.2.3" >&2; exit 1; }
cd "$(dirname "$0")/../../.."
(cd daemon && { [ -d node_modules ] || pnpm install --frozen-lockfile >/dev/null; } && npm run --silent build >/dev/null)
stage=$(mktemp -d); trap 'rm -rf "$stage" "$stage.txt"' EXIT
mkdir -p "$stage/daemon" "$stage/robot" "$stage/deploy"
cp -R daemon/dist daemon/package.json daemon/pnpm-lock.yaml "$stage/daemon/"
cp deploy/jetson/update/pincer-update.sh deploy/jetson/update/pincer-rollback.sh "$stage/deploy/"
rsync -a --exclude .venv --exclude '.*_cache' --exclude '__pycache__' --exclude '*.egg-info' --exclude tests robot/ "$stage/robot/"
printf '%s\n' "$ver" > "$stage/VERSION"
(cd "$stage" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 | sed 's|  \./|  |' > "$stage.txt")
# manifest.json: {"version":..,"created":..,"files":{"path":"sha256",...}}
python3 - "$stage" "$ver" <<'PY'
import json, sys, time
stage, ver = sys.argv[1], sys.argv[2]
files = {}
for line in open(f"{stage}.txt"):
    h, p = line.rstrip("\n").split("  ", 1)
    files[p] = h
json.dump({"version": ver, "created": int(time.time()), "files": files}, open(f"{stage}/manifest.json", "w"), indent=1, sort_keys=True)
PY
rm -f "$stage.txt"
[ "$key" = "-" ] || openssl pkeyutl -sign -inkey "$key" -rawin -in "$stage/manifest.json" -out "$stage/manifest.sig"
tar -C "$stage" -czf "$out/pincer1-$ver.tar.gz" .
echo "$out/pincer1-$ver.tar.gz"
