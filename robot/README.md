# pincer_robot

The robot-side service of **Pincer1** (Lamport Robotics). It runs on the hexapod's main
computer, an **NVIDIA Jetson Orin Nano Super 8 GB**, and answers one question for
`pincerd`, continuously: *is the owner physically here right now?* It fuses the 2D lidar,
the camera (owner face + liveness), the microphone (passphrase + speaker verification)
and the IMU into the `presence` object of [`docs/PROTOCOL.md`](../docs/PROTOCOL.md)
section 2, speaks for the daemon, moves the body for `attention`, and (optionally)
answers the owner's `chat` through a small companion brain.

It is **not** part of the trust boundary. The consent controller and the SE050 are.
This service stops a remote attacker from reaching the touch step and protects the
owner from social engineering; it never sees the key.

```
                      ws://127.0.0.1:7402
 pincerd (daemon)  <----------------------->  pincer-robot
                                                 |-- /scan  (ROS2 LaserScan)  -> presence/lidar.py
                                                 |-- /imu   (ROS2 Imu)        -> fusion "moved"
                                                 |-- camera (OpenCV+insightface, CUDA) -> presence/face.py
                                                 |-- mic    (faster-whisper CUDA + resemblyzer) -> presence/voice.py, duress.py
                                                 |-- speaker (ElevenLabs / espeak-ng) <- speech.py
                                                 |-- brain  (Claude / local llama.cpp) <- brain.py
                                                 |-- INA226 (I2C, pack V/A)    -> battery.py, dock.py
                                                 `-- /cmd_vel (Twist)          <- motion.py
```

The same code runs on a laptop in `--mock` mode (no hardware, no GPU, no torch) and on a
Raspberry Pi 5 as a CPU fallback; `PINCER_DEVICE=auto` picks the backend.

## Layout

```
pincer_robot/
  config.py            pydantic-settings; PINCER_* env + .env, ELEVENLABS_* / ANTHROPIC_API_KEY
  compute.py           PINCER_DEVICE auto|cuda|cpu -> ComputePlan (ort providers, model sizes, torch device)
  brain.py             Brain.reply(): AnthropicBrain / LocalBrain (OpenAI-compatible) / NoBrain + guardrails
  hashing.py           scrypt hashes of spoken secrets (exact + phonetic key)
  duress.py            duress word in STT output, fuzzy, latched, silent
  speech.py            TTS (ElevenLabs stream / espeak-ng / print, urgency for the alarm) + microphone
  motion.py            RobotInterface / MockRobot (ClaudeGO pattern) + attention / idle sway
  ros_backend.py       HexapodRobot: rclpy for /cmd_vel, /scan, /imu
  nav.py               slam_toolbox / Nav2 (lazy rclpy + nav2_simple_commander) + MockNav; room registry,
                       goto, find_owner, map-based room lock
  airgap.py            air-gap relay on a Jetson GPIO line (Jetson.GPIO lazy; mock elsewhere): off/strict/night
  battery.py           INA226 pack monitor (smbus2 lazy; scriptable mock): OCV -> SoC, 2 s voltage filter, charging
  dock.py              optional charging dock: enrolment (dock.json + "dock" room), idle/navigating/approaching/
                       verifying/charging/undocking/failed state machine, battery_low / dock_failed events
  service.py           websockets server, presence push, say / ask_passphrase / attention / morning_report / chat,
                       settings / enroll_room / network / touch / wake / goto / name_room / map / dock, events
  enroll.py            `pincer-enroll` (+ `--add-person`) + the enrolment file model
  main.py              `pincer-robot` wiring, mock sensor simulation
  presence/
    lidar.py           background model + clustering + motion tracking -> confidence, bearing, persons
    room.py            room fingerprint from the lidar background (72-bin polygon-centred profile + wall
                       segments), rotation/translation tolerant room_match
    face.py            FaceRecognizer (insightface CUDA/CPU / mock), OwnerFaceMatcher (multi-owner), LivenessGate
    voice.py           STT + speaker verification -> PassphraseVerifier
    fusion.py          pure state machine -> PresenceState (hysteresis, grace, timeouts, moved, duress)
    synthetic.py       ray-cast room generator shared by tests and --mock
tests/                 pytest, no hardware, no torch/onnx/anthropic
```

## Install

### 1. Jetson Orin Nano Super (the robot)

**Flash.** JetPack 6.x (L4T r36, Ubuntu 22.04, CUDA 12.x, TensorRT 10, Python 3.10) via
NVIDIA SDK Manager or the SD-card image; the Pincer1-specific first-boot steps (controller,
daemon, pairing) are in [`docs/SETUP.md`](../docs/SETUP.md). After flashing:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo nvpmodel -m 0            # MAXN SUPER (25 W) - the Orin Nano Super's full clocks
sudo jetson_clocks            # pin clocks; add to a oneshot unit if you want it on every boot
sudo nvpmodel -q              # verify; `tegrastats` shows GPU load while the models run
```

Thermals: the Nano Super at MAXN wants the active fan; `jetson_clocks` pins it on. If the
body shell is tight, `nvpmodel -m 1` (15 W) still runs everything below, a little slower.

**ROS2 Humble** (the chassis' lidar, IMU and gait talk ROS2) comes from apt on 22.04:

```bash
sudo apt install -y software-properties-common curl && sudo add-apt-repository universe
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu jammy main" | sudo tee /etc/apt/sources.list.d/ros2.list
sudo apt update && sudo apt install -y ros-humble-ros-base python3-colcon-common-extensions
echo 'source /opt/ros/humble/setup.bash' >> ~/.bashrc
```

Bring up the chassis' lidar / IMU / gait nodes (the Pincer1 ROS2 workspace, ported from
the Pi image), then verify topic names: `ros2 topic list | grep -iE 'cmd_vel|scan|imu'`.
Override with `PINCER_SCAN_TOPIC`, `PINCER_IMU_TOPIC`, `PINCER_CMD_VEL_TOPIC` if they differ.

**Camera.** Either a CSI camera on the Orin Nano's 22-pin connector (IMX219/IMX477;
`nvarguscamerasrc` exposes it, and JetPack 6 also maps it to `/dev/video0` for OpenCV
V4L2) or any UVC USB webcam (`/dev/video0`, `v4l2-ctl --list-devices`). Set
`PINCER_CAMERA_INDEX`. A USB camera is the simpler path; CSI only pays off if you want
the ISP's low-light handling.

**USB lidar.** The chassis' 2D lidar (Slamtec RPLIDAR C1) is a USB-serial device; its ROS2 driver publishes
`/scan`. Give it a stable name with the commented example in
`deploy/jetson/udev/99-pincer1.rules` (`/dev/pincer-lidar`, group `dialout`; the service
user is in that group). The consent controller's `/dev/pincer-consent` is *not* reachable
from this service on purpose; only `pincerd` talks to it.

**Install the service** (idempotent; creates user `pincer`, `/opt/pincer1/robot`, venv, unit):

```bash
cd ~/pincer1
deploy/jetson/robot/install.sh
sudo -e /etc/pincer1/robot.env                       # keys, PINCER_BRAIN, topics
sudo -u pincer -H /opt/pincer1/robot/.venv/bin/pincer-enroll   # face, voice, passphrase, duress
sudo systemctl restart pincer-robot && journalctl -u pincer-robot -f
```

What the script does with the Python packages, because it matters on this board:

* The venv is `--system-site-packages` so ROS2's `rclpy`/`sensor_msgs` (apt) and JetPack's
  `cv2` are visible.
* `torch`, `onnxruntime-gpu` and a CUDA build of `ctranslate2` are installed **first, from
  NVIDIA's Jetson wheel index** (`https://pypi.jetson-ai-lab.dev/jp6/cu126` by default;
  override with `PINCER_JETSON_INDEX` to match `cat /etc/nv_tegra_release`). PyPI has no
  aarch64/Tegra builds of these: PyPI's `onnxruntime-gpu` will not install, PyPI's
  `onnxruntime` and `ctranslate2` are CPU-only, and PyPI's `torch` is a CPU wheel. If you
  skip this step everything still runs, silently on the CPU.
* Then `pip install ".[jetson]"` fills in `insightface`, `faster-whisper`, `resemblyzer`,
  `opencv-python-headless`, `sounddevice`, `elevenlabs`, `anthropic` from PyPI (see the
  `[jetson]` extra in `pyproject.toml` for the per-package notes).

Check the backend it picked:

```bash
sudo -u pincer -H /opt/pincer1/robot/.venv/bin/python -c \
  "from pincer_robot.config import Settings; print(Settings().compute_plan().describe())"
# device=cuda face=buffalo_l ort=CUDAExecutionProvider/CPUExecutionProvider whisper=small@cuda/float16 speaker=torch:cuda
```

Models: `buffalo_l` (~275 MB) downloads into `/var/lib/pincer1/.insightface` on first run,
faster-whisper `small` into `/var/lib/pincer1/.cache/huggingface`. First start is slow;
the unit allows 180 s.

**Companion brain (optional).** The owner picks one of four: `PINCER_BRAIN=lamport` runs Lamport-8B on the robot through llama.cpp (offline, no key; see `brain/`), `claude` uses Claude
(`PINCER_BRAIN_MODEL`, default `claude-sonnet-5-5`, key in `ANTHROPIC_API_KEY`).
`openai` uses ChatGPT models (`OPENAI_API_KEY`, `PINCER_OPENAI_MODEL`), `deepseek` uses DeepSeek (`DEEPSEEK_API_KEY`, `PINCER_DEEPSEEK_MODEL`). Whichever brain is chosen, it only talks: no security decision comes from it. `lamport` (alias `local`) talks to an OpenAI-compatible server on the Jetson, e.g.

```bash
# llama.cpp with CUDA (build once on the Jetson), a 3B instruct model in Q4 fits next to the vision models:
llama-server -m ~/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf --port 8080 -ngl 99 -c 2048
# or ollama: PINCER_LOCAL_URL=http://127.0.0.1:11434/v1 PINCER_LOCAL_MODEL=qwen2.5:3b
```

Memory budget on the 8 GB board: `buffalo_l` + whisper `small` fp16 + resemblyzer take
roughly 1.5-2 GB of the shared RAM; a Q4 3B model adds ~2.5 GB. The unit caps the robot
service at 4 GB (`MemoryMax`), the local LLM runs outside that cap. Leave the brain at
`none` until presence works; it is companionship, not a control.

### 2. Laptop (mock mode, runs the whole service and the test-suite)

```bash
cd robot
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
pytest
ruff check .
pincer-robot --mock                 # ws://127.0.0.1:7402 with a synthetic owner in the room
pincer-robot --mock --scenario stranger
pincer-robot --mock --scenario empty
```

Mock mode needs only `websockets`, `numpy`, `pydantic`, `pydantic-settings`. It replays a
ray-cast room through the *same* lidar clustering code the robot uses, a scripted face
(enrolled embedding with head wobble, or a stranger), a silent microphone whose STT
"hears" the mock passphrase (`PINCER_MOCK_PASSPHRASE`, default `open sesame`), and a
logging body. The lidar background learns for 3 s instead of 30 s in mock mode.
`PINCER_DEVICE` is irrelevant in mock mode (no model is loaded); `PINCER_BRAIN=lamport`
with a laptop `llama-server`/ollama works in mock mode and is the easiest way to try `chat`.

Talk to it from a shell:

```bash
python3 - <<'EOF'
import asyncio, json
from websockets.asyncio.client import connect
async def main():
    async with connect("ws://127.0.0.1:7402") as ws:
        print(await ws.recv())                       # presence on connect
        await ws.send(json.dumps({"t":"say","id":"x1","text":"0.5 SOL to a new address. Touch to sign."}))
        await ws.send(json.dumps({"t":"ask_passphrase","id":"x2"}))
        await ws.send(json.dumps({"t":"attention"}))
        await ws.send(json.dumps({"t":"morning_report","id":"r1","items":["SOL up 3 percent.",{"title":"Agent","text":"Closed two trades."}]}))
        await ws.send(json.dumps({"t":"chat","id":"c1","text":"what did I miss?"}))   # error unless PINCER_BRAIN is set
        for _ in range(9): print(await ws.recv())
asyncio.run(main())
EOF
```

### 3. Raspberry Pi 5 (CPU fallback; Ubuntu 22.04 + ROS2 Humble)

```bash
sudo apt install python3-venv python3-dev portaudio19-dev espeak-ng libgl1 libglib2.0-0
cd ~/pincer1/robot
python3 -m venv --system-site-packages .venv     # system site so rclpy/sensor_msgs are visible
source /opt/ros/humble/setup.bash
source .venv/bin/activate
pip install -e ".[pi,dev]"
```

Everything in `[pi]` comes from PyPI. `PINCER_DEVICE=auto` resolves to `cpu` (no CUDA
runtime), which selects `buffalo_sc` (~16 MB), faster-whisper `base` int8 and resemblyzer
on CPU torch (~1 GB wheel, slow pip, fine). The same `pincer-robot.service` works with
the paths adjusted; drop `render` from `SupplementaryGroups`.

Notes that apply to both boards:

* `rclpy`, `sensor_msgs`, `geometry_msgs` come from the ROS2 apt install, **not** pip. The
  `[ros]` extra is intentionally empty and exists to document that.
* Resemblyzer is optional at runtime: without it the voice check is passphrase-only and
  logs a warning.
* `PINCER_FACE_MODEL`, `PINCER_WHISPER_MODEL`, `PINCER_WHISPER_COMPUTE_TYPE` override the
  per-device defaults (e.g. `buffalo_l` on the Pi if you accept ~1 fps; `medium` on the
  Jetson if the room is noisy). `PINCER_FORCE_NO_CUDA=1` makes `auto` pick CPU without
  touching the install, for A/B latency checks.

### Expected latencies

**Estimates until measured on the robot.** Derived from published benchmarks of the same
models on the Orin Nano (Super, MAXN) / Pi 5 / a 2023 laptop CPU; the first real numbers
replace this table.

| step | Jetson Orin Nano Super (CUDA) | Raspberry Pi 5 (CPU) | laptop `--mock` |
| --- | --- | --- | --- |
| face detect + embed, 640x480, one face (`buffalo_l` / `buffalo_sc`) | ~25-40 ms (20-30 fps possible; capped at `camera_fps` 8) | ~150-250 ms (`buffalo_sc`, 4-6 fps) | 0 (scripted) |
| passphrase STT, 5 s clip (`small` fp16 / `base` int8) | ~0.4-0.8 s | ~2-4 s | 0 (mock STT) |
| continuous listen chunk, 4 s (`vad_filter`) | ~0.3-0.6 s | ~1.5-3 s | 0 |
| speaker embedding, 5 s (resemblyzer) | ~50-100 ms (torch CUDA) | ~0.5-1 s | 0 |
| lidar clustering per scan (numpy, 360-720 beams) | < 2 ms | < 5 ms | < 5 ms |
| `ask_passphrase` end to end (prompt TTS + 5 s record + STT + speaker) | ~7-8 s, dominated by the 5 s recording | ~9-11 s | ~5 s |
| `chat` reply, Claude Sonnet (network) | ~1-3 s | ~1-3 s | ~1-3 s |
| `chat` reply, local Q4 3B via llama.cpp CUDA, 60 tokens | ~1.5-3 s | not recommended | depends on laptop |
| cold start (model load) | ~20-40 s | ~30-60 s | < 1 s |

The presence loop itself ticks at 10 Hz and the daemon sees changes within one tick plus
the sensor latency above.

### Why insightface (+ onnxruntime) and not face_recognition

`face_recognition` wraps dlib: a 128-d ResNet from 2017, HOG/CNN detection, compiled
from source on ARM, slow at CNN quality, and no head pose beyond 68 landmarks you have to
fit yourself. insightface gives a SCRFD detector plus a 512-d ArcFace embedding through
onnxruntime, with the `CUDAExecutionProvider` on the Jetson (`buffalo_l`, proper 3D head
pose) and the CPU provider on a Pi (`buffalo_sc`, 5-keypoint pose estimate), far better
cross-pose accuracy, and the pose variation the liveness gate needs. The stored embeddings
are 512-d ArcFace vectors; enrol and run with the same pack.

## Configuration

Environment variables (or a `.env` next to where you launch; `/etc/pincer1/robot.env` under
systemd), all `PINCER_*` except the ElevenLabs and Anthropic keys. See
`pincer_robot/config.py` for the full list and defaults.

| variable | default | meaning |
| --- | --- | --- |
| `PINCER_DEVICE` | `auto` | `auto` / `cuda` / `cpu`; auto = CUDA when onnxruntime, torch or ctranslate2 see a GPU |
| `PINCER_FACE_MODEL` | per device | insightface pack: `buffalo_l` on cuda, `buffalo_sc` on cpu |
| `PINCER_WHISPER_MODEL`, `PINCER_WHISPER_COMPUTE_TYPE` | per device | `small`/`float16` on cuda, `base`/`int8` on cpu |
| `PINCER_ENROL_PATH` | `~/.pincer1/enrol.json` | enrolment file (`/var/lib/pincer1/.pincer1/enrol.json` under systemd) |
| `PINCER_WS_PORT` | `7402` | daemon link (host is always 127.0.0.1) |
| `PINCER_OWNER_NAME` | `owner` | |
| `PINCER_PASSPHRASE_HASH` / `PINCER_DURESS_HASH` | unset | override the enrolment hashes (`pincer-enroll --hash "phrase"`) |
| `PINCER_SCAN_TOPIC` / `PINCER_IMU_TOPIC` / `PINCER_CMD_VEL_TOPIC` | `/scan` `/imu` `/cmd_vel` | |
| `PINCER_CAMERA_INDEX`, `PINCER_CAMERA_HFOV_DEG` | `0`, `62` | |
| `PINCER_SPEAKER_THRESHOLD` | `0.75` | resemblyzer cosine for "same speaker" |
| `PINCER_LIDAR_LEARN_S` | `30` | background learning window |
| `PINCER_OWNER_GRACE_S`, `PINCER_SESSION_TIMEOUT_S`, `PINCER_VOICE_TTL_S` | `5`, `300`, `120` | fusion timeouts |
| `PINCER_TTS_BACKEND` | `auto` | `elevenlabs` / `espeak` / `print` |
| `ELEVENLABS_API_KEY`, `ELEVENLABS_VOICE_ID` | unset | required for ElevenLabs TTS |
| `PINCER_BRAIN` | `none` | `anthropic` / `local` / `none` for the `chat` request |
| `PINCER_BRAIN_MODEL`, `ANTHROPIC_API_KEY` | `claude-sonnet-5-5`, unset | Claude model and key |
| `PINCER_LOCAL_URL`, `PINCER_LOCAL_MODEL` | `http://127.0.0.1:8080/v1`, `default` | OpenAI-compatible local server |
| `PINCER_BRAIN_MAX_TOKENS`, `PINCER_BRAIN_TIMEOUT_S` | `300`, `30` | |
| `PINCER_ROOM_LOCK`, `PINCER_TWO_PERSON`, `PINCER_AIRGAP`, `PINCER_WAKE` | `0`, `0`, `off`, `0` | boot defaults of the owner-optional features; the daemon's `settings` message changes them at runtime |
| `PINCER_ROOM_PATH` | `~/.pincer1/room.json` | room fingerprint (`enroll_room`) |
| `PINCER_AIRGAP_PIN`, `PINCER_AIRGAP_ACTIVE_HIGH`, `PINCER_AIRGAP_GRACE_S` | `7`, `1`, `60` | relay BOARD pin on the 40-pin header, polarity, strict-mode grace |
| `PINCER_WAKE_PAUSE_S`, `PINCER_WAKE_STEP_M`, `PINCER_WAKE_MAX_SECS` | `2.0`, `0.20`, `120` | wake-up alarm pacing |
| `PINCER_MAPS_DIR`, `PINCER_ROOMS_PATH` | `~/.pincer1/maps`, `~/.pincer1/rooms.json` | Nav2 map (`map` save) and named rooms |
| `PINCER_NAV_GOAL_TIMEOUT_S`, `PINCER_NAV_SEARCH_DWELL_S`, `PINCER_NAV_ROOM_RADIUS_M` | `90`, `10`, `2.0` | |
| `PINCER_NAV_ROOM_PRIORITY`, `PINCER_NAV_LOCALIZED_COV` | `["bedroom","desk","living"]`, `0.25` | `find_owner` order; AMCL covariance below which the robot counts as localised |
| `PINCER_DOCK` | `0` | boot default of the charging-dock feature (runtime: `settings` `dock`) |
| `PINCER_DOCK_AT_PCT`, `PINCER_UNDOCK_AT_PCT`, `PINCER_BATTERY_LOW_PCT` | `20`, `90`, `15` | go and charge at / leave at / `battery_low` event when it cannot dock |
| `PINCER_BATTERY_I2C_BUS`, `PINCER_BATTERY_I2C_ADDR`, `PINCER_BATTERY_SHUNT_OHM` | `7`, `0x40`, `0.002` | INA226 on the 40-pin header pins 3/5 (`/dev/i2c-7`), A0/A1 to GND, 2 mΩ shunt |
| `PINCER_BATTERY_INVERT_CURRENT`, `PINCER_BATTERY_FILTER_S`, `PINCER_BATTERY_CHARGE_A` | `0`, `2.0`, `0.3` | shunt wired backwards; voltage low-pass; current into the pack that counts as charging |
| `PINCER_DOCK_PATH`, `PINCER_DOCK_APPROACH_M`, `PINCER_DOCK_APPROACH_SPEED_MPS` | `~/.pincer1/dock.json`, `0.5`, `0.05` | enrolment record; straight-back final approach |
| `PINCER_DOCK_VERIFY_S`, `PINCER_DOCK_BACKOFF_M`, `PINCER_DOCK_MAX_ATTEMPTS`, `PINCER_DOCK_UNDOCK_M`, `PINCER_DOCK_RETRY_COOLDOWN_S` | `10`, `0.2`, `3`, `0.3`, `900` | |
| `PINCER_MOCK_BATTERY_PCT`, `PINCER_MOCK_BATTERY_DRAIN_S` | `55`, `1800` | the `--mock` pack: start SoC and full-to-empty time |

No secret has a default and none is in the code.

## Enrolment

```bash
pincer-enroll            # on the robot, with camera + mic
pincer-enroll --mock     # synthetic enrolment for laptop runs
pincer-enroll --hash "open sesame"   # print the two hashes (exact, phonetic) for an env override
pincer-enroll --add-person spouse    # one more face (label) for the two-person rule; face only
```

The interactive flow:

1. Owner name.
2. **Passphrase** (2+ words, typed twice, never echoed) and **duress word** (1-2 words).
   Both are stored only as scrypt hashes, twice each: of the normalised text and of a crude
   phonetic key (`hashing.phonetic_key`), so Whisper's usual slips (`pine apple`,
   `pinapple`) still match. Word counts are stored so the live check knows the window size.
3. **Face**: 10 frames, each accepted only if the head pose differs by >= 8 degrees from
   every frame already kept (prompts: straight, left, right, chin up, chin down). The
   pack name used (`buffalo_l` / `buffalo_sc`) is recorded; run with the same one.
4. **Voice**: the passphrase spoken three times; the mean resemblyzer d-vector is stored.
   If Whisper hears something other than the passphrase, the heard variant's hashes are
   added as accepted alternatives so your STT's habits do not lock you out.

`enrol.json` is written with mode `0600`. The **embeddings are stored raw**: they are not
secrets (they cannot fool the live checks on their own) but they are personal data; treat
the file like a password-manager export and keep it on the robot only.

## Running

```bash
pincer-robot                # real sensors; needs ROS2 sourced and an enrolment
pincer-robot --mock         # laptop
pincer-robot --mock --scenario stranger --port 7412 --log-level DEBUG
```

What the daemon sees (PROTOCOL.md section 2):

* `presence` on connect, immediately when any boolean changes, and every 2 s otherwise.
  Confidence-only changes ride on the heartbeat.
* `say` -> speaks, then `{"t":"said","id"}`. `ask_passphrase` -> "Say the passphrase.",
  5 s recording, `{"t":"passphrase","id","ok"}`. `attention` -> turn toward the owner
  (lidar bearing, else face x-position) and step forward 10 cm. `morning_report` ->
  items read one by one with a short pause, `{"t":"done","id"}` if an id was given.
  `chat` -> `{"t":"chat","id","text"}` with the brain's reply, then it is spoken;
  `{"t":"error","error":"no_brain..."}` when `PINCER_BRAIN=none`. Unknown types get
  `{"t":"error",...}`. The acks are additive; a daemon can ignore them.
* Duress is never spoken about. It only appears as `duress:true` and is latched.

### Owner-optional features (PROTOCOL.md section 2, "additional states")

All four are **off** until the daemon sends `{"t":"settings",...}` (or the `PINCER_*` boot defaults
say otherwise). Each adds fields to `presence` only while it is on.

| feature | what it does | where |
| --- | --- | --- |
| room lock (`room_lock`) | `enroll_room` fingerprints the lidar *background* (static walls after the learning window): a 72-bin polar range profile taken around the polygon centroid of the walls (translation-tolerant to ~0.5 m) plus the sorted wall-segment lengths; `room_match` is the best of 72 rotations, soft per-bin tolerance 0.10-0.35 m. Synthetic rooms: same room moved 0.5 m and turned >= 0.90, 5 x 3 m vs 4 x 4 m 0.35, a room 0.2 m larger on each side 0.60 (the inherent limit: that looks like a range offset). Identical rooms elsewhere match - it locks against "carried away", not room identity. When a Nav2 map exists, `room_match` comes from localisation instead (1.0 = AMCL converged inside a named room / the map, 0.0 = no fix); the fingerprint is the fallback without a map. | `presence/room.py`, `nav.room_match_from_localization` |
| two-person rule (`two_person`) | `persons` = distinct lidar tracks that moved like a person (confidence >= 0.5); `owners` = enrolled labels (`pincer-enroll --add-person`) matched strongly within 3 s - two people need not share a frame. `owner` stays the primary owner's liveness-gated match; the daemon applies the rule (`persons >= 2` and two labels). | `presence/lidar.py`, `presence/face.py` |
| air-gap relay (`airgap`) | a relay on BOARD pin 7 powers the USB Wi-Fi: `off` = always on; `strict` = on while the owner is present + 60 s (daemon `network on` ignored, `network off` honoured); `night` = strict + daemon windows (`network on … for_secs`). `network` in `presence` is the applied state. Radio left on at exit. Jetson.GPIO is imported lazily; off the Jetson the relay is a logged mock. | `airgap.py` |
| wake-up alarm (`wake`) | `wake` request: find the owner if not in view (navigation, see below), turn to their last bearing, step 20 cm, then speak the lines with rising urgency (ElevenLabs stability 0.5 -> 0.1, espeak-ng amplitude 100 -> 200 and +40 % rate, pauses 2 s -> 0.3 s) until a daemon-relayed `touch`, the passphrase heard by the continuous listener, or `max_secs`. `wake_off` immediately when disabled. | `service.wake`, `speech.py` |

### Mapping and navigation (`nav.py`)

`{"t":"map","action":"start"}` launches slam_toolbox (`online_async_launch.py`) as a subprocess; walk or
carry the robot through the home, then `action:"save"` runs `nav2_map_server map_saver_cli` into
`~/.pincer1/maps/home.{yaml,pgm}` and stops mapping. Nav2 itself (`nav2_bringup` with that map and AMCL)
is a systemd unit in `deploy/jetson`, not started by this service. `name_room` stores the current map
pose with a 2 m radius (or a polygon) in `~/.pincer1/rooms.json`; `goto` sends a `BasicNavigator`
goal with a timeout; `find_owner` visits the room where the owner was last seen, then `bedroom`,
`desk`, `living`, then the rest, turning 360 degrees and watching for a person + face for 10 s per
stop. On a laptop `MockNav` teleports, so `pincer-robot --mock` answers every nav message without ROS2.
`presence` gains `room` (name or `null`) whenever a map exists.

### Charging dock (optional; `battery.py`, `dock.py`)

Off until the daemon sends `{"t":"settings","dock":true}` (or `PINCER_DOCK=1`). Independent of the
flag, every `presence` message carries `battery` (`pct`, `voltage`, `charging`) and `dock` (the
controller state, `off` while the feature is disabled) once the service runs; on a laptop that is a
scripted mock pack.

**Parts** (docs/HARDWARE.md has the BOM lines): the 3S4P Li-ion 21700 pack (9.0 V cutoff, 11.1 V
nominal, 12.6 V full; the BMS balances), a TI INA226 current/voltage monitor breakout with a 2 mΩ
shunt, two spring-loaded pogo contacts under the body, a floor dock plate with two copper strips
25 mm apart, a 3S CC/CV charger brick (12.6 V, 3 A) and a Schottky diode rated for 3 A.

**Wiring.**

* INA226 *in series with the pack rail*: the shunt between the BMS output and everything else
  (Jetson, servo bus, bucks), so it sees every ampere in and out. `VBUS` to the pack side of the
  shunt (bus voltage = pack voltage). `SDA`/`SCL` to the Jetson 40-pin header pins 3/5
  (`/dev/i2c-7` on the Orin Nano carrier; pins 27/28 are `/dev/i2c-1`), `GND` common, `A0`/`A1` to
  GND for address `0x40`. Keep the I2C leads short; the breakout's 10 kΩ pull-ups are enough on a
  3.3 V bus. Check with `i2cdetect -y 7`. The service user needs the `i2c` group (`sudo usermod -aG
  i2c pincer`). The sign of the current depends on which side of the shunt the pack is on; the
  code treats **positive as charging (into the pack)** - if `presence` shows negative current
  while on the dock, set `PINCER_BATTERY_INVERT_CURRENT=1` rather than resoldering.
* Pogo pins under the belly plate, 25 mm apart, wired to the pack's charge input **through the
  BMS** (the BMS's charge FET and over-voltage protection stay in the loop; never straight to the
  cells).
* Dock plate: two copper strips 25 mm apart, long enough that a 10-15 cm error along the strips
  still makes contact, screwed to a 3 mm plate that stands **against a wall** (the final approach
  uses the lidar range to that wall). Charger brick (12.6 V, 3 A CC/CV) to the strips through the
  Schottky diode so a robot sitting on the plate with the brick unplugged cannot back-feed it, and a
  reversed robot cannot be fed. Mind the polarity of the strips vs the pogo pins; the diode
  protects the brick, not the pack.
* Nothing on the dock talks to the robot. "I am on the dock" is simply current flowing into the
  pack above `PINCER_BATTERY_CHARGE_A` (0.3 A), which is also why a dead brick reads as "not
  docked".

**State of charge** comes from a 3S open-circuit-voltage table in `battery.py`, linearly
interpolated, on a voltage that is low-passed with a 2 s time constant. It is honest at rest and
pessimistic under load (walking sags the terminal voltage by a few hundred millivolts); the 20 %
trigger therefore fires a little early while moving, which is the safe direction. A coulomb
counter on the same chip can be added later to make the middle of the curve accurate.

**Enrolment.** Build/load the map first. Put the robot ON the dock with its rear to the wall (pogo
pins seated, the brick plugged in so `charging` is `true` in `presence`), then send
`{"t":"name_room","id":"n1","name":"dock"}`. The robot stores its exact map pose and the lidar range
to the wall behind it in `~/.pincer1/dock.json`, and registers a room named `dock` at the *staging
pose* 0.5 m in front of the plate with a 0.3 m radius; that is what `goto("dock")` drives to. Re-send
to re-enrol after moving the plate.

**Behaviour.** At or below `PINCER_DOCK_AT_PCT` (20 %) - and only when nothing more important is
happening: no wake running, no speech or passphrase check in progress, the owner has not called it
with `attention` in the last 30 s, the body is not already moving - it navigates to `dock`, then
backs straight up at 0.05 m/s in 5 cm steps until the rear lidar range equals the enrolled wall
distance (+3 cm) or current flows (or it has gone 0.6 m, the budget), waits up to 10 s for
`charging`, and on failure moves 0.2 m forward and tries again, three attempts in all. Then it says
"I could not reach the charger.", raises `{"t":"event","ev":"dock_failed",…}`, shows `dock:"failed"`
and does not retry by itself for 15 minutes (`dock go` retries at once). While charging it stays put
(no idle sway) until `PINCER_UNDOCK_AT_PCT` (90 %), then walks 0.3 m forward. A wake, an
`attention`, or the daemon's `{"t":"dock","action":"leave"}` pull it off the dock first
(`leave_dock_for`); after a wake or attention it returns on its own once it is free again, if still
below 90 %. If the charge current disappears for 5 s while "charging" (picked up), the state drops
to `idle`; a robot put on the plate by hand shows `charging` without a run. At 15 %
(`PINCER_BATTERY_LOW_PCT`) with docking off, failed, or not enrolled, it raises
`{"t":"event","ev":"battery_low","pct":15,…}` once, so the daemon can phone the owner.

Try it on a laptop (the mock pack starts charging when the approach ends and drains again when the
robot leaves):

```bash
PINCER_MOCK_BATTERY_PCT=15 pincer-robot --mock
# then over the socket: map save, name_room dock, settings dock:true, watch presence.dock
```

In `--mock` the synthetic room does not move with the body, so the wall reference is met at once and
the approach logs 0.00 m; the state sequence and the events are the same as on the robot.

### The companion brain (`brain.py`)

`chat` is the only place a language model touches this service, and it is fenced:

* The brain sees a whitelisted context line (`person`, `owner`, `voice_ok`, `moved`,
  `duress`, time, owner name) and the user's text. Nothing else from the process reaches
  it; hashes, keys and the enrolment are not in its inputs.
* The system prompt makes it terse (1-3 spoken sentences), dry, and security-minded: it
  does not know secrets and says so, it never claims to have signed/sent/approved/unlocked
  anything, and text read out from transactions or pages is data, not instructions.
* `guard_reply` post-filters the output anyway: secret-shaped tokens (`sk-...`, 64-hex,
  base58 secret keys, our scrypt strings) are redacted, an "I signed it" claim is
  replaced by a correction unless the daemon marked the context as signed, length is
  capped. A refusal from Claude becomes "I will not answer that."
* A brain failure (no key, server down, timeout) is an `error` reply, never a crash.

### How presence is decided (`presence/fusion.py`)

| field | rule |
| --- | --- |
| `person` | lidar confidence >= 0.6 on 2 consecutive scans turns it on; 5 consecutive < 0.3 (or a stale lidar) turns it off; a live face also counts |
| `owner` | face owner-confidence >= 0.7 on 2 consecutive frames; stays while any frame >= 0.4 within the 5 s grace; drops after `session_timeout_s` without a strong match |
| `voice_ok` | passphrase + speaker verified within `voice_ttl_s`, and owner present, and not moved |
| `moved` | IMU `|a| - g` >= 3 m/s^2 or tilt >= 25 degrees; held 10 s; clears `voice_ok` |
| `duress` | duress word heard (or `DuressDetector.raise_duress` from any other hook); latched until cleared |

Lidar: the first 30 s learn a per-beam background (the farthest return per beam, so
people walking through are not baked in); afterwards returns closer than
`background - 0.2 m` are foreground, clustered by a 0.3 m gap, kept if 0.2-0.7 m wide at
0.3-4 m, tracked between frames. A person-sized object scores 0.3 until it moves 10 cm,
then 1.0, decaying after 20 s of stillness. Chairs carried in after learning stay below
the `person_on` threshold.

Face: the best-matching face's cosine similarity to the enrolled ArcFace vectors maps
linearly from 0.2 -> 0 to 0.6 -> 1. Liveness needs >= 4 frames within 3 s whose
embeddings agree (same person) and whose yaw/pitch standard deviation is >= 3 degrees;
otherwise confidence is multiplied by 0.25, below `owner_on`. A printed photo or a
phone held still fails. A replayed video of the owner moving does not; that is what the
passphrase/speaker check and the physical touch are for.

## Testing

```bash
pytest            # no hardware needed
ruff check .
```

Covered: every fusion transition (hysteresis, grace, session timeout, voice TTL,
moved from accel / tilt / explicit hook, duress latch, wire format), lidar clustering on
synthetic rooms (walls, static chair, walking person with bearing error < 5 degrees,
person present during learning, wide object, range limits, 360-degree wrap), face
liveness (static photo rejected, head motion accepted, stranger low, identity swap),
passphrase verification with mocks, duress fuzzy match, hashing, enrolment round-trip,
motion safety envelope, device selection (`auto`/`cuda`/`cpu`, per-device model
defaults, overrides, half-installed CUDA, probes with every runtime absent), the brain
(selection from env, missing key/package, system-prompt rules, context whitelist,
secret redaction, signing-claim veto, Anthropic request shape against a fake client,
refusal handling, the local provider against a real loopback HTTP server, HTTP 500 and
unreachable), and the websocket protocol end to end with a real client (presence on
connect / on change / heartbeat, say, ask_passphrase, attention, morning_report, chat
reply-then-speak with whitelisted context, serialised speech, garbage input, reconnect
mid-speech), and the owner-optional features: room fingerprint on synthetic rooms (same room
rotated / moved 0.3 and 0.5 m >= 0.85, three different rooms <= 0.5, furniture moved >= 0.75, a
walker during learning ignored, save/load), person counting (two bodies -> 2, one -> 1, a chair ->
0), multi-owner face labels (both labels, one, stranger none, window ageing), fusion wire fields on/off
and staleness, air-gap mode logic and override expiry with a mock relay plus a fake `Jetson.GPIO`
module (BOARD pin, polarity), the `settings` message (partial update, validation, ack), `enroll_room`
/ `network` acks, the wake loop (touch stops it, passphrase stops it, timeout, `wake_off`, rising
urgency, travel via a fake `find_owner`), navigation (room registry radius/polygon/persistence, visit
ordering, `find_owner` with a teleporting `MockNav` incl. blocked rooms and the time budget,
`room_match` from a fake localisation covariance/score, `goto` / `name_room` / `map` messages), and the
charging dock: the OCV table (monotonic, endpoints, inverse), charging detection at 0.3 A and the current
inversion, the 2 s voltage filter, sensor errors, the INA226 register decoding against a fake `smbus2`
(byte order, signed shunt, config word) and the fallback to the mock pack; the dock state machine against a
simulated plate (enrolment writes `dock.json` + the 0.3 m `dock` room at the staging pose, the happy path
navigate → approach on the wall distance → charging → undock at 90 %, a Nav2 short-stop corrected by the
lidar reference with a retry, three failed attempts → `failed` + speech + `dock_failed` + sticky until the
cooldown, nav failure, `busy()` blocking, manual `go`, a wake pulling it off and the automatic return, the
daemon's `leave` not returning, interruption mid-run, `battery_low` when off / not enrolled / failed with
re-arm, the enable toggle, pick-up detection, hand placement, and the stale-sample guard after an undock),
plus the `dock` request, the `settings` `dock` key, event broadcast and `battery`/`dock` on the wire.

