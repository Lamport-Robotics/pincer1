#!/usr/bin/env bash
# Install this checkout on the robot as an unsigned local release (used by the daemon and robot install
# scripts; a shipped update is a signed tarball through pincer-update.sh instead). Idempotent per commit.
set -euo pipefail
cd "$(dirname "$0")/../../.."
ROOT=${PINCER_ROOT:-/opt/pincer1}
sha=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
git diff --quiet 2>/dev/null || sha="$sha.$(date +%Y%m%d%H%M%S)"
ver="0.0.0+local.$sha"
sudo mkdir -p "$ROOT/releases"
sudo install -m 755 deploy/jetson/update/pincer-update.sh /usr/local/sbin/pincer-update
sudo install -m 755 deploy/jetson/update/pincer-rollback.sh /usr/local/sbin/pincer-rollback
if [ -d "$ROOT/releases/$ver" ]; then echo "release $ver already installed"; exit 0; fi
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
rel=$(deploy/jetson/update/make-release.sh "$ver" - "$out")
# services may not exist yet on a first install: health is skipped, the units are started by the caller
sudo PINCER_ROOT="$ROOT" PINCER_ALLOW_UNSIGNED=1 PINCER_HEALTH=true PINCER_HEALTH_TIMEOUT=1 PINCER_SYSTEMCTL="${PINCER_SYSTEMCTL:-true}" \
  PINCER_SKIP_DEPS="${PINCER_SKIP_DEPS:-}" /usr/local/sbin/pincer-update "$rel"
