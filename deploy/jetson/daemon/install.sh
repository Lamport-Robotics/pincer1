#!/usr/bin/env bash
# Installs pincerd on the Jetson (Ubuntu 22.04). Idempotent. Run from the repo root as a sudoer.
set -euo pipefail
cd "$(dirname "$0")/../../.." || exit 1
# pincerd runs as its own user with its own state directory; the controller's serial port belongs to the
# pincer-consent group, which holds only pincerd (the robot service, user pincer, stays out of it)
getent group pincer-consent >/dev/null || sudo groupadd --system pincer-consent
id -u pincerd >/dev/null 2>&1 || sudo useradd --system --create-home --home-dir /var/lib/pincerd --groups pincer-consent pincerd
sudo usermod -aG pincer-consent pincerd
sudo mkdir -p /opt/pincer1/releases /etc/pincer1 /var/lib/pincerd
sudo install -m 644 deploy/jetson/udev/99-pincer1.rules /etc/udev/rules.d/99-pincer1.rules && sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=tty || true
command -v node >/dev/null || { curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -; sudo apt-get install -y nodejs; }
command -v pnpm >/dev/null || sudo npm i -g pnpm@10
# the daemon and the robot package ship together as one release under /opt/pincer1/releases/<version>,
# with /opt/pincer1/current pointing at the live one (deploy/jetson/update). A checkout install is an
# unsigned local release; field updates are signed tarballs from make-release.sh.
(cd daemon && CI=true pnpm install --frozen-lockfile)
deploy/jetson/update/install-local.sh
[ -f /etc/pincer1/daemon.env ] || sudo install -m 600 deploy/jetson/daemon/daemon.env.example /etc/pincer1/daemon.env
sudo install -m 644 deploy/jetson/daemon/pincerd.service /etc/systemd/system/pincerd.service
sudo chown -R pincerd:pincerd /var/lib/pincerd && sudo chmod 750 /var/lib/pincerd
sudo systemctl daemon-reload && sudo systemctl enable --now pincerd
systemctl --no-pager --lines=5 status pincerd
