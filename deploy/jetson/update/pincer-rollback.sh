#!/usr/bin/env bash
# Switch /opt/pincer1/current back to the previous release and restart the services.
#   sudo pincer-rollback.sh            # to the release recorded by the last update
#   sudo pincer-rollback.sh 1.2.2      # to a named installed release
set -euo pipefail
ROOT=${PINCER_ROOT:-/opt/pincer1}; SYSTEMCTL=${PINCER_SYSTEMCTL:-systemctl}
SERVICES=(pincer-robot pincerd)
log() { echo "[pincer-rollback] $*" >&2; }
if [ -n "${1:-}" ]; then target="$ROOT/releases/$1"; else target=$(cat "$ROOT/.previous" 2>/dev/null || true); fi
[ -n "$target" ] && [ -d "$target" ] || { log "no previous release to roll back to (installed: $(ls "$ROOT/releases" 2>/dev/null | tr '\n' ' '))"; exit 2; }
cur=$(readlink "$ROOT/current" 2>/dev/null || true)
[ "$cur" = "$target" ] && { log "already on $target"; exit 0; }
if [ -z "${PINCER_SKIP_DEPS:-}" ] && [ -x "$ROOT/robot-venv/bin/pip" ] && [ -d "$target/robot" ]; then
  (cd "$target/robot" && "$ROOT/robot-venv/bin/pip" install -q --no-deps .) || log "warning: robot package reinstall failed; the venv keeps the newer package"
fi
ln -sfn "$target" "$ROOT/current.new" && mv -Tf "$ROOT/current.new" "$ROOT/current" 2>/dev/null || { rm -f "$ROOT/current.new"; ln -sfn "$target" "$ROOT/current"; }
[ -n "$cur" ] && echo "$cur" > "$ROOT/.previous"
log "current -> $target; restarting ${SERVICES[*]}"
for s in "${SERVICES[@]}"; do $SYSTEMCTL restart "$s" || true; done
