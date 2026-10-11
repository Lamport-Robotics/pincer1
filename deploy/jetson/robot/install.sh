#!/usr/bin/env bash
# Installs pincer-robot on a Jetson Orin Nano (JetPack 6.x, Ubuntu 22.04, Python 3.10).
# Idempotent: re-run after a `git pull`. Run from anywhere as a sudoer; it cds to the repo root.
#
# What it does
#   1. apt deps: venv, portaudio (sounddevice), espeak-ng (offline TTS), libs for OpenCV/insightface.
#   2. system user `pincer` (shared with pincerd), /opt/pincer1/robot, /etc/pincer1, /var/lib/pincer1.
#   3. rsync robot/ into /opt/pincer1/robot (no .venv, caches or tests).
#   4. venv with --system-site-packages so ROS2 Humble's rclpy and JetPack's OpenCV are visible.
#   5. CUDA wheels from NVIDIA's Jetson index FIRST (torch, onnxruntime-gpu, ctranslate2) - these are
#      not on PyPI for aarch64/Tegra - then `pip install ".[jetson]"` which fills in the rest from PyPI.
#   6. /etc/pincer1/robot.env (kept if present), the systemd unit, enable + start.
#
# Env knobs
#   PINCER_JETSON_INDEX  wheel index for your JetPack (default: pypi.jetson-ai-lab.dev/jp6/cu126).
#                        Check https://pypi.jetson-ai-lab.dev for the one matching `cat /etc/nv_tegra_release`.
#   PINCER_SKIP_CUDA=1   skip step 5 (CPU-only install, e.g. to test the unit wiring first).
set -euo pipefail
cd "$(dirname "$0")/../../.." || exit 1
REPO="$(pwd)"
APP=/opt/pincer1/robot
INDEX="${PINCER_JETSON_INDEX:-https://pypi.jetson-ai-lab.dev/jp6/cu126}"

if [ ! -f /etc/nv_tegra_release ] && [ -z "${PINCER_SKIP_CUDA:-}" ]; then
  echo "warning: this does not look like a Jetson (/etc/nv_tegra_release missing)." >&2
  echo "         set PINCER_SKIP_CUDA=1 for a CPU-only install, or use pip install -e '.[pi]' by hand." >&2
fi

# 1. apt
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
  python3-venv python3-dev python3-pip build-essential rsync \
  portaudio19-dev libsndfile1 espeak-ng alsa-utils \
  libgl1 libglib2.0-0 libgomp1 \
  v4l-utils

# 2. user + dirs (same `pincer` user as deploy/jetson/daemon/install.sh; harmless if it exists)
id -u pincer >/dev/null 2>&1 || sudo useradd --system --create-home --home-dir /var/lib/pincer1 pincer
for g in video audio dialout render; do
  getent group "$g" >/dev/null && sudo usermod -aG "$g" pincer || true
done
sudo mkdir -p "$APP" /etc/pincer1 /var/lib/pincer1
sudo chown pincer:pincer /var/lib/pincer1
sudo chmod 750 /var/lib/pincer1

# 3. code
sudo rsync -a --delete \
  --exclude .venv --exclude '.*_cache' --exclude '__pycache__' --exclude '*.egg-info' --exclude tests \
  "$REPO/robot/" "$APP/"

# 4. venv (system site: rclpy / sensor_msgs / geometry_msgs from ROS2 apt, cv2 from JetPack)
[ -x "$APP/.venv/bin/python" ] || sudo python3 -m venv --system-site-packages "$APP/.venv"
PIP="sudo $APP/.venv/bin/pip"
$PIP install -q --upgrade pip wheel

# 5. CUDA wheels from NVIDIA's index, before the PyPI extras so pip does not pull CPU builds.
if [ -z "${PINCER_SKIP_CUDA:-}" ]; then
  echo "installing CUDA wheels from $INDEX"
  # torch for resemblyzer (CPU torch from PyPI would otherwise satisfy the dependency).
  $PIP install -q --index-url "$INDEX" --extra-index-url https://pypi.org/simple torch torchaudio
  # onnxruntime-gpu for insightface's CUDAExecutionProvider (PyPI has no Tegra build).
  $PIP install -q --index-url "$INDEX" --extra-index-url https://pypi.org/simple onnxruntime-gpu
  # CUDA ctranslate2 for faster-whisper float16 (PyPI's aarch64 wheel is CPU-only).
  $PIP install -q --index-url "$INDEX" --extra-index-url https://pypi.org/simple ctranslate2 || \
    echo "warning: no CUDA ctranslate2 on $INDEX; faster-whisper will run int8 on CPU" >&2
fi

# 6. the package + the Jetson extra (everything on PyPI)
(cd "$APP" && $PIP install -q ".[jetson]")

# config + unit
[ -f /etc/pincer1/robot.env ] || sudo install -m 600 -o pincer -g pincer "$REPO/deploy/jetson/robot/robot.env.example" /etc/pincer1/robot.env
sudo install -m 644 "$REPO/deploy/jetson/robot/pincer-robot.service" /etc/systemd/system/pincer-robot.service
sudo systemctl daemon-reload
sudo systemctl enable --now pincer-robot
systemctl --no-pager --lines=5 status pincer-robot || true

cat <<EOF

pincer-robot installed.
  config   /etc/pincer1/robot.env        (PINCER_DEVICE, PINCER_BRAIN, ELEVENLABS_*, topics)
  enrol    sudo -u pincer -H $APP/.venv/bin/pincer-enroll   (writes /var/lib/pincer1/.pincer1/enrol.json)
  logs     journalctl -u pincer-robot -f
  check    sudo -u pincer -H $APP/.venv/bin/python -c "from pincer_robot.config import Settings; print(Settings().compute_plan().describe())"
Performance: sudo nvpmodel -m 0 && sudo jetson_clocks   (MAXN + pinned clocks; see robot/README.md)
EOF
