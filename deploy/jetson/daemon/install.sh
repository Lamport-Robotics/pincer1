#!/usr/bin/env bash
# Installs pincerd on the Jetson (Ubuntu 22.04). Idempotent. Run from the repo root as a sudoer.
set -euo pipefail
cd "$(dirname "$0")/../../.." || exit 1
id -u pincer >/dev/null 2>&1 || sudo useradd --system --create-home --home-dir /var/lib/pincer1 --groups dialout pincer
sudo mkdir -p /opt/pincer1/daemon /etc/pincer1 /var/lib/pincer1
command -v node >/dev/null || { curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -; sudo apt-get install -y nodejs; }
command -v pnpm >/dev/null || sudo npm i -g pnpm@10
(cd daemon && pnpm install --frozen-lockfile && pnpm build)
sudo rsync -a --delete daemon/dist daemon/package.json daemon/node_modules /opt/pincer1/daemon/
[ -f /etc/pincer1/daemon.env ] || sudo install -m 600 deploy/jetson/daemon/daemon.env.example /etc/pincer1/daemon.env
sudo install -m 644 deploy/jetson/daemon/pincerd.service /etc/systemd/system/pincerd.service
sudo chown -R pincer:pincer /var/lib/pincer1
sudo systemctl daemon-reload && sudo systemctl enable --now pincerd
systemctl --no-pager --lines=5 status pincerd
