#!/usr/bin/env bash
# Regenerate robot/requirements.lock: the runtime pins from pyproject.toml with sha256 hashes of the
# wheels for Python 3.10 and 3.11 on aarch64 and x86_64 (Jetson + CI). Needs network and a pip >= 23.
#   robot/tools/lock.sh            # writes requirements.lock next to pyproject.toml
set -euo pipefail
cd "$(dirname "$0")/.."
PIP=${PIP:-python3 -m pip}
PINS=$(python3 - <<'PY'
import re, tomllib
d = tomllib.load(open("pyproject.toml", "rb"))["project"]["dependencies"]
print(" ".join(d))
PY
)
# transitive runtime deps, pinned here so the lock is complete (pip resolves nothing at install time)
PINS="$PINS pydantic-core==2.50.1 python-dotenv==1.2.4 typing-extensions==4.16.0 annotated-types==0.8.0 typing-inspection==0.4.4"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
for arch in aarch64 x86_64; do for py in 3.10 3.11; do
  # shellcheck disable=SC2086
  $PIP download -q --no-deps --python-version "$py" --platform "manylinux2014_$arch" --platform "manylinux_2_28_$arch" \
    --only-binary=:all: -d "$tmp" $PINS
done; done
python3 - "$tmp" "$PINS" > requirements.lock <<'PY'
import hashlib, os, sys, collections
root, pins = sys.argv[1], sys.argv[2].split()
hashes = collections.defaultdict(list)
for f in sorted(os.listdir(root)):
    hashes[f.split("-")[0].replace("_", "-").lower()].append(hashlib.sha256(open(os.path.join(root, f), "rb").read()).hexdigest())
print("# Runtime dependencies of pincer-robot, pinned with wheel hashes for Python 3.10 (Jetson, JetPack 6)")
print("# and 3.11 (CI), aarch64 and x86_64. Regenerate: robot/tools/lock.sh. Installed with --require-hashes by")
print("# deploy/jetson/robot/install.sh and deploy/jetson/update/pincer-update.sh before the package itself.")
for pin in sorted(pins):
    name = pin.split("==")[0].lower()
    hs = hashes[name]
    assert hs, name
    print(f"\n{pin} \\")
    print(" \\\n".join(f"    --hash=sha256:{h}" for h in hs))
PY
echo "wrote requirements.lock ($(grep -c -- --hash requirements.lock) hashes)"
