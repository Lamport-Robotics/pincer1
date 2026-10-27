#!/usr/bin/env bash
# Install a signed release on the robot, switch to it, check it is healthy, roll back if it is not.
#   sudo pincer-update.sh pincer1-1.2.3.tar.gz
# Layout: /opt/pincer1/releases/<version>/{daemon,robot,VERSION}  /opt/pincer1/current -> releases/<version>
# The services run from /opt/pincer1/current; switching is one symlink rename, so a failed health check
# puts the previous release back in seconds. The robot venv is shared (CUDA wheels are large); the
# release's robot package is installed into it and the previous package reinstalled on rollback.
#
# Overrides (tests, unusual boxes): PINCER_ROOT (/opt/pincer1), PINCER_ETC (/etc/pincer1),
# PINCER_SYSTEMCTL (systemctl), PINCER_HEALTH (command that must exit 0 once the new release is up),
# PINCER_HEALTH_TIMEOUT (90 s), PINCER_SKIP_DEPS=1 (no pnpm/pip), PINCER_ALLOW_UNSIGNED=1 (local builds).
set -euo pipefail
tarball=${1:?release tarball}
ROOT=${PINCER_ROOT:-/opt/pincer1}; ETC=${PINCER_ETC:-/etc/pincer1}
SYSTEMCTL=${PINCER_SYSTEMCTL:-systemctl}
HEALTH=${PINCER_HEALTH:-"curl -fsS --max-time 3 http://127.0.0.1:7401/status"}
HEALTH_TIMEOUT=${PINCER_HEALTH_TIMEOUT:-90}
SERVICES=(pincer-robot pincerd)
log() { echo "[pincer-update] $*" >&2; }

# 1. unpack to a scratch dir and verify before anything runs
tmp=$(mktemp -d "$ROOT/.update.XXXXXX" 2>/dev/null || mktemp -d); trap 'rm -rf "$tmp"' EXIT
tar -C "$tmp" -xzf "$tarball"
[ -f "$tmp/manifest.json" ] || { log "no manifest.json in the release"; exit 2; }
if [ -z "${PINCER_ALLOW_UNSIGNED:-}" ]; then
  [ -f "$ETC/release.pub" ] || { log "no $ETC/release.pub on this robot; refusing an unverifiable release"; exit 2; }
  [ -f "$tmp/manifest.sig" ] || { log "release is not signed"; exit 2; }
  openssl pkeyutl -verify -pubin -inkey "$ETC/release.pub" -rawin -in "$tmp/manifest.json" -sigfile "$tmp/manifest.sig" >/dev/null 2>&1 \
    || { log "manifest signature does not verify against $ETC/release.pub"; exit 2; }
else
  log "WARNING: signature check skipped (PINCER_ALLOW_UNSIGNED)"
fi
python3 - "$tmp" <<'PY' || exit 2
import hashlib, json, os, sys
root = sys.argv[1]
m = json.load(open(os.path.join(root, "manifest.json")))
listed = set(m["files"])
bad = []
for p, h in m["files"].items():
    f = os.path.join(root, p)
    if not os.path.isfile(f) or hashlib.sha256(open(f, "rb").read()).hexdigest() != h:
        bad.append(p)
present = set()
for d, _, fs in os.walk(root):
    for f in fs:
        rel = os.path.relpath(os.path.join(d, f), root)
        if rel not in ("manifest.json", "manifest.sig"):
            present.add(rel)
extra = sorted(present - listed)
if bad or extra:
    print("[pincer-update] manifest mismatch: bad=%s extra=%s" % (bad[:5], extra[:5]), file=sys.stderr); sys.exit(1)
PY
ver=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['version'])" "$tmp/manifest.json")
[[ $ver =~ ^v?[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$ ]] && [[ $ver != *..* ]] || { log "bad version string: $ver"; exit 2; }
dest="$ROOT/releases/$ver"
[ -e "$dest" ] && { log "release $ver is already installed at $dest (remove it to reinstall)"; exit 2; }

# 2. put it in place and install its dependencies (nothing is switched yet)
mkdir -p "$ROOT/releases"
mv "$tmp" "$dest"; trap - EXIT
chmod 755 "$dest"
if [ -z "${PINCER_SKIP_DEPS:-}" ]; then
  (cd "$dest/daemon" && pnpm install --prod --frozen-lockfile) || { log "pnpm install failed"; rm -rf "$dest"; exit 3; }
  if [ -x "$ROOT/robot-venv/bin/pip" ]; then
    (cd "$dest/robot" && "$ROOT/robot-venv/bin/pip" install -q --require-hashes -r requirements.lock && "$ROOT/robot-venv/bin/pip" install -q ".[jetson]") || { log "pip install failed"; rm -rf "$dest"; exit 3; }
  fi
fi

# 3. switch: remember the previous release, flip the symlink atomically, restart
prev=$(readlink "$ROOT/current" 2>/dev/null || true)
[ -n "$prev" ] && echo "$prev" > "$ROOT/.previous"
ln -sfn "$dest" "$ROOT/current.new" && mv -Tf "$ROOT/current.new" "$ROOT/current" 2>/dev/null || { rm -f "$ROOT/current.new"; ln -sfn "$dest" "$ROOT/current"; }
log "switched to $ver (previous: ${prev:-none}); restarting ${SERVICES[*]}"
for s in "${SERVICES[@]}"; do $SYSTEMCTL restart "$s" || true; done

# 4. health: the daemon must answer /status and both units must stay active
deadline=$(( $(date +%s) + HEALTH_TIMEOUT )); ok=
while [ "$(date +%s)" -lt "$deadline" ]; do
  if sh -c "$HEALTH" >/dev/null 2>&1; then
    active=1; for s in "${SERVICES[@]}"; do $SYSTEMCTL is-active --quiet "$s" || active=; done
    [ -n "$active" ] && { ok=1; break; }
  fi
  sleep 2
done
if [ -n "$ok" ]; then
  log "release $ver is up"
  exit 0
fi
log "release $ver did not become healthy within ${HEALTH_TIMEOUT}s; rolling back"
PINCER_ROOT="$ROOT" PINCER_SYSTEMCTL="$SYSTEMCTL" PINCER_SKIP_DEPS="${PINCER_SKIP_DEPS:-}" "$(dirname "$0")/pincer-rollback.sh" || true
exit 4
