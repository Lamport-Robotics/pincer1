"""Settings, read from the environment and an optional ``.env`` in the working directory.

Every variable is prefixed ``PINCER_`` except the ElevenLabs and Anthropic ones, which
keep their conventional names (``ELEVENLABS_API_KEY``, ``ELEVENLABS_VOICE_ID``,
``ANTHROPIC_API_KEY``). No secret has a default; keys are only ever read from the
environment.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

from pydantic import AliasChoices, Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="PINCER_",
        # a .env beside the code is for a developer's laptop; under systemd the unit's
        # EnvironmentFile is the only source, so a .env that slipped into a release fills nothing
        env_file=None if os.environ.get("INVOCATION_ID") else ".env",
        env_file_encoding="utf-8",
        extra="ignore",
        populate_by_name=True,
    )

    # --- identity / enrolment -------------------------------------------------
    owner_name: str = "owner"
    enrol_path: Path = Path("~/.pincer1/enrol.json")
    # Optional overrides of the enrolment file (scrypt strings from `pincer-enroll --hash`).
    passphrase_hash: str | None = None
    duress_hash: str | None = None

    # --- daemon link -----------------------------------------------------------
    ws_host: str = "127.0.0.1"
    ws_port: int = 7402
    heartbeat_s: float = 2.0

    # --- ROS2 topics (verify with `ros2 topic list` on the robot) -------------------
    scan_topic: str = "/scan"
    imu_topic: str = "/imu"
    cmd_vel_topic: str = "/cmd_vel"

    # --- legs ------------------------------------------------------------------------
    # twist: hand velocity commands to an external gait controller on cmd_vel_topic (demo chassis).
    # bus:   drive the leg servos directly with the gait in pincer_robot.body (the v1 body).
    body_backend: Literal["twist", "bus"] = "twist"
    servo_port: str = "/dev/ttyUSB0"
    servo_baud: int = 1_000_000
    gait: Literal["tripod", "ripple", "wave"] = "tripod"
    # servo health (bus backend): every poll reads temperature and load of each joint; past a
    # threshold the body refuses to walk for cooldown_s and says why in the log
    servo_health_poll_s: float = 5.0
    servo_max_temp_c: int = 70
    servo_max_load: int = 800  # of ±1000
    servo_cooldown_s: float = 30.0

    # --- compute backend ---------------------------------------------------------
    # auto: CUDA when onnxruntime / torch / ctranslate2 see a GPU (Jetson Orin Nano),
    # else CPU (Pi 5, laptop). Per-device defaults for the model names below live in
    # compute.py; an explicit PINCER_FACE_MODEL / PINCER_WHISPER_MODEL always wins.
    device: Literal["auto", "cuda", "cpu"] = "auto"

    # --- camera / face -----------------------------------------------------------
    camera_index: int = 0
    camera_hfov_deg: float = 62.0
    camera_fps: float = 8.0
    face_model: str | None = None  # buffalo_l on cuda, buffalo_sc on cpu
    face_det_size: int = 320
    face_sim_low: float = 0.20  # cosine similarity that maps to confidence 0
    face_sim_high: float = 0.60  # cosine similarity that maps to confidence 1
    face_not_live_factor: float = 0.25
    liveness_window_s: float = 3.0
    liveness_min_frames: int = 4
    liveness_min_pose_std_deg: float = 3.0
    liveness_min_embed_sim: float = 0.70

    # --- microphone / voice --------------------------------------------------------
    mic_sample_rate: int = 16000
    mic_device: str | None = None
    listen_chunk_s: float = 4.0
    # how long the robot keeps reporting duress after the word (then it listens again)
    duress_hold_s: float = 900.0
    listen_energy_threshold: float = 0.01
    passphrase_record_s: float = 5.0
    whisper_model: str | None = None  # small on cuda, base on cpu
    whisper_compute_type: str | None = None  # float16 on cuda, int8 on cpu
    whisper_language: str | None = None
    speaker_threshold: float = 0.75
    voice_ttl_s: float = 120.0

    # --- lidar person detector -----------------------------------------------------
    lidar_learn_s: float = 30.0
    lidar_bg_margin_m: float = 0.20
    lidar_cluster_gap_m: float = 0.30
    lidar_min_points: int = 3
    lidar_person_width_min_m: float = 0.20
    lidar_person_width_max_m: float = 0.70
    lidar_person_range_min_m: float = 0.30
    lidar_person_range_max_m: float = 4.0
    lidar_move_threshold_m: float = 0.10
    lidar_still_hold_s: float = 20.0

    # --- fusion ---------------------------------------------------------------------
    person_on: float = 0.6
    person_off: float = 0.3
    person_on_frames: int = 2
    person_off_frames: int = 5
    lidar_stale_s: float = 3.0
    owner_on: float = 0.7
    owner_off: float = 0.4
    owner_on_frames: int = 2
    owner_grace_s: float = 5.0
    session_timeout_s: float = 300.0
    moved_accel_threshold: float = 3.0  # m/s^2 away from 1 g
    moved_tilt_deg: float = 25.0
    moved_hold_s: float = 10.0
    face_stale_s: float = 2.0

    # --- speech ---------------------------------------------------------------------
    tts_backend: Literal["auto", "elevenlabs", "espeak", "print"] = "auto"
    elevenlabs_api_key: str | None = Field(
        default=None, validation_alias=AliasChoices("ELEVENLABS_API_KEY", "elevenlabs_api_key")
    )
    elevenlabs_voice_id: str | None = Field(
        default=None, validation_alias=AliasChoices("ELEVENLABS_VOICE_ID", "elevenlabs_voice_id")
    )
    elevenlabs_model: str = "eleven_flash_v2_5"
    report_pause_s: float = 0.7

    # --- companion brain (optional `chat` request, PROTOCOL.md section 2) -------------
    # none: `chat` answers with an error. anthropic: Claude via the `anthropic` package
    # (ANTHROPIC_API_KEY). local: an OpenAI-compatible server on the Jetson (llama.cpp
    # `llama-server`, ollama) at PINCER_LOCAL_URL.
    # The owner picks the brain: lamport (Lamport-8B on the robot, offline), claude, openai
    # (ChatGPT), deepseek. "local"/"anthropic" are accepted aliases. Security decisions never
    # come from the brain, whichever one is chosen.
    brain: Literal["lamport", "claude", "openai", "deepseek", "local", "anthropic", "none"] = "none"
    brain_model: str = "claude-sonnet-5-5"
    brain_max_tokens: int = 300
    brain_timeout_s: float = 30.0
    anthropic_api_key: str | None = Field(
        default=None, validation_alias=AliasChoices("ANTHROPIC_API_KEY", "anthropic_api_key")
    )
    local_url: str = "http://127.0.0.1:8080/v1"
    local_model: str = "lamport-8b"
    openai_api_key: str | None = Field(
        default=None, validation_alias=AliasChoices("OPENAI_API_KEY", "openai_api_key")
    )
    openai_model: str = "gpt-4o"
    deepseek_api_key: str | None = Field(
        default=None, validation_alias=AliasChoices("DEEPSEEK_API_KEY", "deepseek_api_key")
    )
    deepseek_model: str = "deepseek-chat"

    # --- owner-optional features (all OFF by default; runtime `settings` message) ------
    room_lock: bool = False  # publish room_match (fingerprint or map localisation)
    room_path: Path = Path("~/.pincer1/room.json")
    room_match_interval_s: float = 0.5
    two_person: bool = False  # publish persons + owners
    airgap: Literal["off", "strict", "night"] = "off"
    airgap_pin: int = 7  # BOARD numbering on the Jetson 40-pin header
    airgap_active_high: bool = True
    airgap_grace_s: float = 60.0
    wake: bool = False  # accept the daemon's wake-up alarm
    wake_pause_s: float = 2.0  # pause after the first line; shrinks as urgency rises
    wake_step_m: float = 0.20
    wake_max_secs: int = 120

    # --- navigation (slam_toolbox / Nav2 on the chassis' ROS2 stack) -------------------
    maps_dir: Path = Path("~/.pincer1/maps")
    rooms_path: Path = Path("~/.pincer1/rooms.json")
    nav_goal_timeout_s: float = 90.0
    nav_search_dwell_s: float = 10.0
    nav_room_radius_m: float = 2.0
    nav_room_priority: list[str] = ["bedroom", "desk", "living"]
    nav_localized_cov: float = 0.25  # pose covariance (x/y variance) below which AMCL "has" us

    # --- battery + charging dock (optional; `dock` flag OFF by default) -----------------
    # 3S4P Li-ion 21700 pack (9.0 V cutoff, 11.1 V nominal, 12.6 V full) measured by a TI
    # INA226 on the pack rail (I2C, 2 mOhm shunt). Positive current = INTO the pack.
    dock: bool = False  # autonomous docking (needs a map + a registry entry named "dock")
    dock_at_pct: float = 20.0  # go and charge at or below this state of charge
    undock_at_pct: float = 90.0  # leave the dock at or above this
    battery_low_pct: float = 15.0  # `battery_low` event when docking is off / failed
    battery_i2c_bus: int = 7  # Orin Nano 40-pin header pins 3/5 = /dev/i2c-7 (27/28 = i2c-1)
    battery_i2c_addr: int = 0x40  # INA226 with A0/A1 to GND
    battery_shunt_ohm: float = 0.002
    battery_invert_current: bool = False  # flip if the shunt is wired the other way round
    battery_filter_s: float = 2.0  # voltage low-pass time constant
    battery_charge_a: float = 0.3  # current into the pack above which "charging" is true
    dock_path: Path = Path("~/.pincer1/dock.json")  # on-dock pose + wall distance
    dock_approach_m: float = 0.5  # straight-back final approach from the registry pose
    dock_approach_speed_mps: float = 0.05
    dock_verify_s: float = 10.0  # wait for current after the approach
    dock_backoff_m: float = 0.2  # forward before a retry
    dock_max_attempts: int = 3
    dock_undock_m: float = 0.3  # forward when leaving
    dock_retry_cooldown_s: float = 900.0  # after `failed`, try again on its own after this

    # --- mock mode -------------------------------------------------------------------
    mock_scenario: Literal["owner", "stranger", "empty"] = "owner"
    mock_passphrase: str = "open sesame"
    mock_duress_word: str = "pineapple"
    mock_battery_pct: float = 55.0  # starting state of charge of the mock pack
    mock_battery_drain_s: float = 1800.0  # mock pack: 100 % -> 0 % over this many seconds

    @property
    def enrol_file(self) -> Path:
        return self.enrol_path.expanduser()

    def feature_flags(self) -> FeatureFlags:
        return FeatureFlags(
            room_lock=self.room_lock,
            two_person=self.two_person,
            airgap=self.airgap,
            wake=self.wake,
            dock=self.dock,
        )

    def lidar_config(self):  # noqa: ANN201 - avoids importing numpy-heavy module at config import
        from .presence.lidar import LidarConfig

        return LidarConfig(
            learn_seconds=self.lidar_learn_s,
            bg_margin=self.lidar_bg_margin_m,
            cluster_gap=self.lidar_cluster_gap_m,
            min_points=self.lidar_min_points,
            person_width=(self.lidar_person_width_min_m, self.lidar_person_width_max_m),
            person_range=(self.lidar_person_range_min_m, self.lidar_person_range_max_m),
            move_threshold=self.lidar_move_threshold_m,
            still_hold_s=self.lidar_still_hold_s,
        )

    def fusion_config(self):  # noqa: ANN201
        from .presence.fusion import FusionConfig

        return FusionConfig(
            person_on=self.person_on,
            person_off=self.person_off,
            person_on_frames=self.person_on_frames,
            person_off_frames=self.person_off_frames,
            lidar_stale_s=self.lidar_stale_s,
            owner_on=self.owner_on,
            owner_off=self.owner_off,
            owner_on_frames=self.owner_on_frames,
            owner_grace_s=self.owner_grace_s,
            session_timeout_s=self.session_timeout_s,
            voice_ttl_s=self.voice_ttl_s,
            moved_accel_threshold=self.moved_accel_threshold,
            moved_tilt_deg=self.moved_tilt_deg,
            moved_hold_s=self.moved_hold_s,
            face_stale_s=self.face_stale_s,
        )

    def compute_plan(self):  # noqa: ANN201 - lazy; compute.py probes CUDA runtimes
        from .compute import build_plan, resolve_device

        return build_plan(
            resolve_device(self.device),
            face_model=self.face_model,
            whisper_model=self.whisper_model,
            whisper_compute_type=self.whisper_compute_type,
        )

    def face_config(self):  # noqa: ANN201
        from .presence.face import FaceConfig

        return FaceConfig(
            sim_low=self.face_sim_low,
            sim_high=self.face_sim_high,
            not_live_factor=self.face_not_live_factor,
            window_s=self.liveness_window_s,
            min_frames=self.liveness_min_frames,
            min_pose_std_deg=self.liveness_min_pose_std_deg,
            min_embed_sim=self.liveness_min_embed_sim,
        )


@dataclass
class FeatureFlags:
    """The owner-optional features, switchable at runtime by the daemon's
    ``{"t":"settings",...}`` message. Lives in memory only; the env gives the defaults."""

    room_lock: bool = False
    two_person: bool = False
    airgap: str = "off"
    wake: bool = False
    dock: bool = False
    # offline: radio at least strict, Lamport-8B only. online: radio on, any brain, swaps and agent.
    mode: str = "online"
    brain: str = ""  # "" = keep the env default (PINCER_BRAIN); else lamport|claude|openai|deepseek

    def update(self, msg: dict[str, Any]) -> list[str]:
        """Apply the keys present in ``msg``; returns the names that changed.

        Unknown keys are ignored, bad values raise ``ValueError`` (nothing is applied).
        """
        new = dict(vars(self))
        for key in ("room_lock", "two_person", "wake", "dock"):
            if key in msg:
                if not isinstance(msg[key], bool):
                    raise ValueError(f"{key} must be a boolean")
                new[key] = msg[key]
        if "airgap" in msg:
            if msg["airgap"] not in ("off", "strict", "night"):
                raise ValueError("airgap must be off|strict|night")
            new["airgap"] = msg["airgap"]
        if "mode" in msg:
            if msg["mode"] not in ("offline", "online"):
                raise ValueError("mode must be offline|online")
            new["mode"] = msg["mode"]
        if "brain" in msg:
            if msg["brain"] not in ("", "lamport", "claude", "openai", "deepseek", "none"):
                raise ValueError("brain must be lamport|claude|openai|deepseek|none")
            new["brain"] = msg["brain"]
        changed = [k for k, v in new.items() if getattr(self, k) != v]
        for k, v in new.items():
            setattr(self, k, v)
        return changed

    def to_message(self) -> dict[str, Any]:
        return {"t": "settings", **vars(self)}


def load_settings(**overrides: object) -> Settings:
    """Settings from env/.env with explicit keyword overrides (used by the CLIs)."""
    return Settings(**overrides)  # type: ignore[arg-type]
