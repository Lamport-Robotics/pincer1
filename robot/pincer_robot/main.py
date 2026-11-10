"""``pincer-robot``: wire sensors -> fusion -> daemon service.

Real mode (Jetson Orin Nano, or a Pi as CPU fallback): ROS2 subscribers for /scan and
/imu, OpenCV camera through insightface, the microphone through faster-whisper +
resemblyzer, ElevenLabs TTS, /cmd_vel motion, and the optional companion brain for
``chat``. ``compute.ComputePlan`` (PINCER_DEVICE) decides CUDA vs CPU for the models.
Every heavy import is lazy and lives in its own module, so ``pincer-robot --mock`` runs
on a laptop with only websockets/numpy/pydantic.

Mock mode replays a synthetic room (same clustering code as the robot), a scripted
owner/stranger face, a silent microphone whose STT "hears" the mock passphrase, and
a logging robot body. ``--scenario owner|stranger|empty`` picks what the room contains.

The owner-optional features (room lock, two-person rule, air-gap relay, wake-up alarm,
navigation, charging dock) are wired here too: ``FeatureFlags`` (env defaults, daemon
``settings`` at runtime), ``AirGap`` on a GPIO relay (mock off the Jetson), ``RoomMatcher``
on the lidar background, ``Navigator`` over Nav2 (``MockNav`` without ROS2),
``BatteryMonitor`` on an INA226 (scripted mock pack elsewhere) and ``DockController``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import threading
import time
from dataclasses import dataclass, field
from types import SimpleNamespace
from typing import Any

import numpy as np

from . import __version__
from .airgap import AirGap, make_relay
from .battery import BatteryMonitor, BatteryState, MockBatterySensor, make_battery_sensor
from .brain import Brain, BrainError, LocalBrain, NoBrain, clean_speech, make_brain, redact_secrets
from .config import FeatureFlags, Settings, load_settings
from .dock import DOCK_ROOM, DockConfig, DockController, DockStore, rear_range_from_scan
from .duress import DuressDetector
from .enroll import Enrolment, build_mock_enrolment, load_enrolment
from .motion import MockRobot, MotionController, RobotInterface, RobotStatus
from .nav import FindResult, Navigator, NavResult, RoomRegistry, make_nav_backend
from .presence.face import (
    FaceObservation,
    FaceRecognizer,
    MockFaceRecognizer,
    OwnerFaceMatcher,
    make_mock_observation,
)
from .presence.fusion import PresenceFusion
from .presence.lidar import PersonDetector, Scan
from .presence.room import RoomMatcher
from .presence.voice import (
    MockSpeakerEncoder,
    MockSTT,
    PassphraseVerifier,
    SpeakerEncoder,
    SpeechToText,
    rms,
)
from .service import RobotService
from .speech import Microphone, Speaker, make_microphone, make_speaker

log = logging.getLogger("pincer.main")


@dataclass
class _Shared:
    """Everything the sensor threads and the asyncio loop both touch."""

    lock: threading.Lock = field(default_factory=threading.Lock)
    last_bearing: float | None = None
    last_bearing_ts: float = 0.0
    face_x_norm: float | None = None
    face_ts: float = 0.0
    speaking: bool = False
    # when the last utterance ended; listening ignores chunks that overlap it
    spoke_until: float = 0.0
    room_match: float | None = None  # fingerprint path (lidar thread writes it)
    room_match_ts: float = 0.0
    rear_range: float | None = None  # lidar range straight behind (dock approach)
    rear_range_ts: float = 0.0
    battery: BatteryState | None = None  # latest pack sample (feature-extras thread)
    last_attention_ts: float = -math.inf
    floor: Any = None  # depth.FloorReport: clearance and the first riser ahead (depth thread)


class RobotApp:
    def __init__(self, cfg: Settings, *, mock: bool) -> None:
        self.cfg = cfg
        self.mock = mock
        self.enrol = self._load_enrolment()
        self.fusion = PresenceFusion(cfg.fusion_config())
        lidar_cfg = cfg.lidar_config()
        if mock and cfg.lidar_learn_s == 30.0:
            lidar_cfg.learn_seconds = 3.0  # don't make a laptop demo wait half a minute
        self.lidar = PersonDetector(lidar_cfg)
        self.face_matcher = OwnerFaceMatcher(
            self.enrol.face_matrix(),
            cfg.face_config(),
            label=self.enrol.owner_name,
            people=self.enrol.people_faces(),
        )
        self.flags: FeatureFlags = cfg.feature_flags()
        self.fusion.two_person = self.flags.two_person
        self.room = RoomMatcher(cfg.room_path)
        self.airgap = AirGap(
            make_relay(cfg.airgap_pin, cfg.airgap_active_high, mock=mock),
            mode=self.flags.airgap,
            grace_s=cfg.airgap_grace_s,
        )
        self.nav = Navigator(
            make_nav_backend(cfg.maps_dir, mock=mock),
            RoomRegistry(cfg.rooms_path),
            cfg.maps_dir,
            goal_timeout_s=cfg.nav_goal_timeout_s,
            dwell_s=cfg.nav_search_dwell_s,
            room_radius_m=cfg.nav_room_radius_m,
            priority=cfg.nav_room_priority,
            localized_cov=cfg.nav_localized_cov,
        )
        self.duress = DuressDetector(
            self.enrol.duress_hashes, self.enrol.duress_words, on_duress=self._on_duress
        )
        self.shared = _Shared()
        self.battery = BatteryMonitor(
            make_battery_sensor(
                mock=mock,
                bus=cfg.battery_i2c_bus,
                addr=cfg.battery_i2c_addr,
                shunt_ohm=cfg.battery_shunt_ohm,
                mock_pct=cfg.mock_battery_pct,
                mock_drain_s=cfg.mock_battery_drain_s,
            ),
            tau_s=cfg.battery_filter_s,
            charge_threshold_a=cfg.battery_charge_a,
            invert_current=cfg.battery_invert_current,
        )
        self.speaker: Speaker = make_speaker(cfg, mock=mock)
        self.mic: Microphone = make_microphone(cfg, mock=mock)
        self._mic_lock = threading.Lock()
        self._stt: SpeechToText | None = None
        self._encoder: SpeakerEncoder | None = None
        self._verifier: PassphraseVerifier | None = None
        self.head: Any = None  # body.GazeHead on the v1 body: the camera arm follows the face
        self.panel: Any = None  # body.pulse.RosbridgePanel on the chassis: oled, buzzer, buttons
        self._panel_reset: asyncio.TimerHandle | None = None  # the pending return to idle
        self.robot: RobotInterface = self._make_robot()
        self.motion = MotionController(self.robot)
        self.dock = DockController(
            DockConfig(
                dock_at_pct=cfg.dock_at_pct,
                undock_at_pct=cfg.undock_at_pct,
                low_pct=cfg.battery_low_pct,
                approach_m=cfg.dock_approach_m,
                approach_speed_mps=cfg.dock_approach_speed_mps,
                verify_s=cfg.dock_verify_s,
                backoff_m=cfg.dock_backoff_m,
                max_attempts=cfg.dock_max_attempts,
                undock_m=cfg.dock_undock_m,
                retry_cooldown_s=cfg.dock_retry_cooldown_s,
            ),
            battery=self.battery_state,
            nav=self.nav,
            robot=self.robot,
            motion=self.motion,
            rear_range=self.rear_range,
            busy=self.busy,
            say=self.say,
            emit=self._emit_event,
            store=DockStore(cfg.dock_path),
            enabled=self.flags.dock,
            on_state=self._on_dock_state if mock else None,
        )
        self._plan: Any | None = None
        self.brain: Brain = self._make_brain()
        self._lamport: LocalBrain | None = None  # offline fallback, built on first use
        self._fallback_reason: str | None = None
        self.service = RobotService(
            self,
            host=cfg.ws_host,
            port=cfg.ws_port,
            heartbeat_s=cfg.heartbeat_s,
            report_pause_s=cfg.report_pause_s,
            flags=self.flags,
            wake_pause_s=cfg.wake_pause_s,
            wake_max_secs=cfg.wake_max_secs,
        )
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []

    # -- construction ----------------------------------------------------------------------
    def _load_enrolment(self) -> Enrolment:
        path = self.cfg.enrol_file
        if path.exists():
            enrol = load_enrolment(path)
            log.info("enrolment loaded for %s from %s", enrol.owner_name, path)
        elif self.mock:
            enrol = build_mock_enrolment(self.cfg)
            log.info("mock enrolment (passphrase %r)", self.cfg.mock_passphrase)
        else:
            raise SystemExit(f"no enrolment at {path}; run `pincer-enroll` first")
        if self.cfg.passphrase_hash:
            enrol.passphrase_hashes = [self.cfg.passphrase_hash]
        if self.cfg.duress_hash:
            enrol.duress_hashes = [self.cfg.duress_hash]
        return enrol

    def _make_robot(self) -> RobotInterface:
        if self.mock:
            return MockRobot()
        if self.cfg.body_backend == "bus":
            from .body import (
                Body,
                BodyBackend,
                BodyGeometry,
                Bus,
                BusActuator,
                GaitParams,
                SerialTransport,
                ServoHealth,
            )

            bus = Bus(SerialTransport(self.cfg.servo_port, self.cfg.servo_baud))
            actuator = BusActuator(bus, BodyGeometry())
            body = Body(gait=GaitParams(name=self.cfg.gait), actuator=actuator)
            body.actuator.torque(True)  # type: ignore[union-attr]
            body.stand()
            log.info("body: own gait (%s) on %s", self.cfg.gait, self.cfg.servo_port)
            from .body import Gaze, GazeConfig, GazeHead

            self.head = GazeHead(bus, Gaze(GazeConfig(hfov_deg=self.cfg.camera_hfov_deg)))
            self.head.raise_head()
            status = RobotStatus(note="body: own gait")
            sensors: Any = None
            try:
                from .ros_backend import SensorNode

                sensors = SensorNode(
                    scan_topic=self.cfg.scan_topic,
                    imu_topic=self.cfg.imu_topic,
                    on_scan=self.on_scan,
                    on_imu=self.on_imu,
                    status=status,
                )
            except ImportError as exc:
                log.error(
                    "ROS2 not available (%s): the own-gait body has NO lidar/IMU, so presence is "
                    "camera and voice only and the moved/tilt checks are inert",
                    exc,
                )
                status.note = "body: own gait, no lidar/imu"
            health = ServoHealth(
                bus,
                actuator.servo_ids(),
                max_temp_c=self.cfg.servo_max_temp_c,
                max_load=self.cfg.servo_max_load,
                cooldown_s=self.cfg.servo_cooldown_s,
            )
            backend = BodyBackend(body, status=status, sensors=sensors, health=health)
            backend.start_health_poll(self.cfg.servo_health_poll_s)
            return backend
        if self.cfg.body_backend == "joint":
            from .body import (
                Body,
                BodyBackend,
                BodyGeometry,
                BusActuator,
                GaitParams,
                Gaze,
                GazeConfig,
                GazeHead,
            )
            from .body.pulse import (
                PulseCalibration,
                RosbridgeClient,
                RosbridgeSensors,
                RosbridgeServoBus,
            )

            try:
                cal = PulseCalibration.load(self.cfg.servo_calibration)
            except (OSError, ValueError, KeyError) as exc:
                if self.cfg.body_profile == "chassis":
                    cal = PulseCalibration.chassis()
                    log.warning(
                        "joint backend: no servo calibration at %s; using the chassis defaults",
                        self.cfg.servo_calibration,
                    )
                else:
                    log.error(
                        "joint backend: no servo calibration at %s (%s); "
                        "run pincer-calibrate first. "
                        "Running with a mock body.",
                        self.cfg.servo_calibration,
                        exc,
                    )
                    return MockRobot()
            try:
                client = RosbridgeClient(self.cfg.rosbridge_url)
            except Exception as exc:
                log.error(
                    "joint backend: rosbridge at %s unreachable (%s); mock body",
                    self.cfg.rosbridge_url,
                    exc,
                )
                return MockRobot()
            bus = RosbridgeServoBus(client, cal)
            geometry = (
                BodyGeometry.chassis() if self.cfg.body_profile == "chassis" else BodyGeometry()
            )
            from .body.pulse import RosbridgePanel

            self.panel = RosbridgePanel(client, on_button=self._on_body_button)
            self.panel.show("PINCER1", "starting")
            actuator = BusActuator(bus, geometry, move_time_ms=40)
            leg_ids = actuator.servo_ids()
            if not cal.complete_for(leg_ids):
                log.error(
                    "joint backend: calibration misses leg servos %s; mock body",
                    [s for s in leg_ids if s not in cal.zero],
                )
                client.close()
                return MockRobot()
            status = RobotStatus(note="body: own gait over rosbridge")
            sensors = RosbridgeSensors(
                client,
                status,
                scan_topic=self.cfg.scan_topic,
                imu_topic=self.cfg.imu_topic,
                on_scan=self.on_scan,
                on_imu=self.on_imu,
                on_tap=self._on_body_tap,
            )
            if not bus.wait_for_state():
                log.warning("joint backend: no servo state from the chassis yet")
            body = Body(geometry=geometry, gait=GaitParams(name=self.cfg.gait), actuator=actuator)
            drive = None
            if self.cfg.body_profile == "chassis" and self.cfg.chassis_gait == "chassis":
                # the chassis walks with its own gait controller; we keep its stance and own the
                # head
                from .body.pulse import RosbridgeDrive

                drive = RosbridgeDrive(client, topic=self.cfg.cmd_vel_topic).drive
                status.note = "body: chassis gait, own head"
                log.info(
                    "body: chassis gait on %s over rosbridge %s",
                    self.cfg.cmd_vel_topic,
                    self.cfg.rosbridge_url,
                )
            else:
                body.stand()
                log.info(
                    "body: own gait (%s) over rosbridge %s", self.cfg.gait, self.cfg.rosbridge_url
                )
            head_cfg = GazeConfig(hfov_deg=self.cfg.camera_hfov_deg)
            if cal.complete_for(
                [head_cfg.pan_id, head_cfg.shoulder_id, head_cfg.elbow_id, head_cfg.tilt_id]
            ):
                self.head = GazeHead(bus, Gaze(head_cfg))
                self.head.raise_head()
            else:
                log.warning("joint backend: arm servos not calibrated, the head stays where it is")
            return BodyBackend(body, status=status, sensors=sensors, health=None, drive=drive)
        try:
            from .ros_backend import HexapodRobot

            return HexapodRobot(
                cmd_vel_topic=self.cfg.cmd_vel_topic,
                scan_topic=self.cfg.scan_topic,
                imu_topic=self.cfg.imu_topic,
                on_scan=self.on_scan,
                on_imu=self.on_imu,
            )
        except ImportError as exc:
            log.error("ROS2 not available (%s): running with a mock body and NO lidar/IMU", exc)
            return MockRobot()

    def _make_brain(self, choice: str | None = None) -> Brain:
        cfg: Any = self.cfg
        if choice:
            fields = {k: getattr(self.cfg, k) for k in type(self.cfg).model_fields}
            cfg = SimpleNamespace(**{**fields, "brain": choice})
        try:
            brain = make_brain(cfg)
        except BrainError as exc:
            log.error("brain disabled: %s", exc)
            return NoBrain()
        if brain.name != "none":
            log.info("companion brain: %s", brain.name)
        return brain

    CLOUD_BRAINS = ("anthropic", "openai", "deepseek")

    def _lamport_brain(self) -> LocalBrain:
        if self._lamport is None:
            self._lamport = LocalBrain(
                self.cfg.local_url,
                self.cfg.local_model,
                max_tokens=self.cfg.brain_max_tokens,
                timeout_s=self.cfg.brain_timeout_s,
                name="lamport",
            )
        return self._lamport

    def active_brain(self) -> tuple[Brain, bool]:
        """The brain to answer with right now and whether we fell back to Lamport-8B.

        Offline mode, or the radio being off for any reason, means a cloud brain cannot be used:
        the robot answers with Lamport-8B instead and says so once per outage.
        """
        radio_off = self.flags.airgap != "off" and not self.airgap.network
        if self.brain.name in self.CLOUD_BRAINS and (self.flags.mode == "offline" or radio_off):
            return self._lamport_brain(), True
        self._fallback_reason = None
        return self.brain, False

    @property
    def plan(self) -> Any:
        """Compute plan (CUDA vs CPU), resolved once on first real-backend use."""
        if self._plan is None:
            self._plan = self.cfg.compute_plan()
            log.info("compute: %s", self._plan.describe())
        return self._plan

    def _voice_backends(self) -> tuple[SpeechToText, SpeakerEncoder | None]:
        if self.mock:
            scenario = self.cfg.mock_scenario
            text = {"owner": self.cfg.mock_passphrase, "stranger": "hello there robot"}.get(
                scenario, ""
            )
            voice = self.enrol.voice_vector()
            if scenario != "owner" or voice is None:
                voice = np.random.default_rng(7).normal(size=256)
            return MockSTT(text), MockSpeakerEncoder(voice)
        from .presence.voice import ResemblyzerEncoder, WhisperSTT

        plan = self.plan
        stt = WhisperSTT.from_plan(plan, self.cfg.whisper_language)
        try:
            enc: SpeakerEncoder | None = ResemblyzerEncoder(plan.torch_device)
        except ImportError:
            log.warning("resemblyzer not installed: passphrase-only voice check")
            enc = None
        return stt, enc

    def _ensure_verifier(self) -> PassphraseVerifier:
        if self._verifier is None:
            self._stt, self._encoder = self._voice_backends()
            self._verifier = PassphraseVerifier(
                self._stt,
                self._encoder,
                self.enrol.passphrase_hashes,
                self.enrol.passphrase_words,
                self.enrol.voice_vector(),
                self.cfg.speaker_threshold,
            )
        return self._verifier

    # -- sensor callbacks (any thread) -------------------------------------------------------
    def on_scan(self, scan: Scan) -> None:
        res = self.lidar.update(scan)
        match: float | None = None
        do_match = (
            self.flags.room_lock
            and not res.learning
            and self.room.enrolled
            and scan.ts - self.shared.room_match_ts >= self.cfg.room_match_interval_s
        )
        if do_match:
            static = self.lidar.static_scan()
            if static is not None:
                match = self.room.match(*static)
        rear = rear_range_from_scan(scan.angles, scan.ranges, scan.valid_mask())
        with self.shared.lock:
            self.fusion.update_lidar(res.confidence, scan.ts, persons=res.persons)
            if res.bearing is not None:
                self.shared.last_bearing = res.bearing
                self.shared.last_bearing_ts = scan.ts
            if match is not None:
                self.shared.room_match = match
                self.shared.room_match_ts = scan.ts
            if rear is not None:
                self.shared.rear_range = rear
                self.shared.rear_range_ts = scan.ts

    def on_imu(self, accel: tuple[float, float, float], tilt_deg: float, ts: float) -> None:
        with self.shared.lock:
            self.fusion.update_imu(accel, ts, tilt_deg)

    def on_faces(self, observations: list[FaceObservation], ts: float) -> None:
        res = self.face_matcher.update(observations)
        with self.shared.lock:
            self.fusion.update_face(
                res.owner_conf, ts, live_present=res.present and res.live, owners=res.owners
            )
            self.shared.face_x_norm = res.x_norm
            self.shared.face_ts = ts
        if self.head is not None:
            try:
                self.head.observe(observations, ts)
            except Exception as exc:  # a bus hiccup must not take the presence loop down
                log.warning("head: %s", exc)

    def on_transcript(self, text: str, audio: np.ndarray | None = None) -> None:
        if not text:
            return
        log.debug("heard: %s", redact_secrets(text))
        self.duress.scan(text)

    def _on_duress(self, source: str) -> None:
        with self.shared.lock:
            self.fusion.raise_duress(time.time(), source)
        # the latch is not forever: after the hold time the robot stops reporting duress, the
        # daemon only ever acts on its rising edge, and the controller leaves the decoy on the next
        # real unlock. Without this the service had to be restarted to hear the word again.
        threading.Timer(self.cfg.duress_hold_s, self._clear_duress).start()

    def _clear_duress(self) -> None:
        with self.shared.lock:
            self.fusion.clear_duress()
        self.duress.reset()
        log.warning("duress latch cleared")

    # -- battery / dock plumbing -----------------------------------------------------------------
    def battery_state(self) -> BatteryState | None:
        with self.shared.lock:
            return self.shared.battery

    def rear_range(self, max_age_s: float = 1.0) -> float | None:
        """Lidar range to whatever is straight behind, or None if stale / no beams."""
        with self.shared.lock:
            if time.time() - self.shared.rear_range_ts > max_age_s:
                return None
            return self.shared.rear_range

    def busy(self) -> bool:
        """Higher-priority work: a wake, speech / passphrase in progress, the owner just
        called it (``attention`` within 30 s), or the body is already moving."""
        return (
            self.service.busy
            or self.motion.busy
            or time.time() - self.shared.last_attention_ts < 30.0
        )

    def _emit_event(self, obj: dict[str, Any]) -> None:
        self.service.emit_event_threadsafe(obj)

    def _on_dock_state(self, state: str) -> None:
        """Mock mode only: the scripted pack starts charging when the approach ends and
        drains again once the robot leaves, so ``--mock`` shows the whole cycle."""
        sensor = self.battery.sensor
        if not isinstance(sensor, MockBatterySensor):
            return
        if state == "verifying":
            sensor.set(current_a=2.5)
            sensor.slope_v_per_s = (12.6 - 9.0) / 300.0  # full in ~5 min for the demo
        elif state in ("undocking", "idle", "off"):
            sensor.set(current_a=-0.9)
            sensor.slope_v_per_s = -(12.6 - 9.0) / max(60.0, self.cfg.mock_battery_drain_s)

    # -- ServiceHandlers --------------------------------------------------------------------
    async def say(self, text: str, urgency: float = 0.0) -> None:
        text = clean_speech(text)  # nothing key-shaped and no control characters reach the speaker
        self.shared.speaking = True
        self.shared.spoke_until = float("inf")
        try:
            await self.speaker.say(text, urgency=urgency)
        finally:
            self.shared.speaking = False
            self.shared.spoke_until = time.time()

    async def ask_passphrase(self) -> bool:
        await self.say("Say the passphrase.")
        verifier = await asyncio.to_thread(self._ensure_verifier)

        def _record_and_check() -> Any:
            with self._mic_lock:
                audio = self.mic.record(self.cfg.passphrase_record_s)
            return verifier.check(audio, self.mic.sample_rate)

        result = await asyncio.to_thread(_record_and_check)
        self.on_transcript(result.transcript)
        with self.shared.lock:
            self.fusion.update_voice(result.ok, time.time())
        log.info(
            "passphrase: phrase_ok=%s speaker_ok=%s score=%.2f",
            result.phrase_ok,
            result.speaker_ok,
            result.speaker_score,
        )
        return result.ok

    def owner_bearing(self, max_age_s: float = 2.0) -> float | None:
        now = time.time()
        with self.shared.lock:
            if (
                self.shared.last_bearing is not None
                and now - self.shared.last_bearing_ts < max_age_s
            ):
                return self.shared.last_bearing
            if self.shared.face_x_norm is not None and now - self.shared.face_ts < max_age_s:
                return -self.shared.face_x_norm * math.radians(self.cfg.camera_hfov_deg) / 2.0
            if self.fusion.last_seen_bearing is not None:
                return self.fusion.last_seen_bearing
        return None

    async def attention(self) -> None:
        self.shared.last_attention_ts = time.time()
        if not self.cfg.attention_moves:
            log.info("attention: head only (attention_moves off)")
            return
        await self.dock.leave_dock_for("attention")
        await self.motion.attention(self.owner_bearing())

    # -- feature handlers ----------------------------------------------------------------------
    async def apply_settings(self, flags: FeatureFlags, changed: list[str]) -> None:
        self.flags = flags
        with self.shared.lock:
            self.fusion.two_person = flags.two_person
            if "room_lock" in changed:
                self.shared.room_match = None
        # offline mode forces the radio to at least strict; online mode leaves airgap as set
        offline_default = flags.mode == "offline" and flags.airgap == "off"
        self.airgap.mode = "strict" if offline_default else flags.airgap
        self.dock.enabled = flags.dock
        if "brain" in changed and flags.brain:
            self.brain = self._make_brain(flags.brain)
            self._fallback_reason = None

    async def enroll_room(self) -> bool:
        static = self.lidar.static_scan()
        if static is None:
            log.warning("enroll_room: lidar background not learned yet")
            return False
        await asyncio.to_thread(self.room.enroll, *static)
        with self.shared.lock:
            self.shared.room_match = 1.0
            self.shared.room_match_ts = time.time()
        log.info("room fingerprint enrolled -> %s", self.room.path)
        return True

    async def network_override(self, on: bool, reason: str, for_secs: float) -> bool:
        return self.airgap.override(on, reason, for_secs, time.time())

    async def wake_approach(self) -> None:
        await self.motion.attention(self.owner_bearing(max_age_s=math.inf), self.cfg.wake_step_m)

    def owner_visible(self) -> bool:
        st = self.service.state
        if st is None:
            return False
        return st.owner or (st.person and st.lidar_conf >= 0.9)

    async def _look_around(self) -> None:
        for _ in range(4):
            await self.motion.attention(math.radians(90.0), 0.0)

    async def find_owner(self, max_secs: float) -> FindResult | None:
        if not self.nav.available:
            return None
        with self.shared.lock:
            last_room = self.fusion.last_seen_room
        return await self.nav.find_owner(
            self.owner_visible,
            last_seen_room=last_room,
            look_around=self._look_around,
            max_secs=max_secs,
        )

    async def goto(self, room: str) -> NavResult:
        return await self.nav.goto(room)

    async def display(self, title: str, body: str, level: str, ttl_secs: float) -> None:
        """The daemon's readout for the panel: on the chassis the two-line OLED, with a beep when a
        request needs the owner."""
        log.info("display: %s | %s (%s)", title, body, level)
        if self.panel is None:
            return
        self.panel.show(title, body)
        if level in ("medium", "high", "block"):
            self.panel.beep(repeat=2 if level != "block" else 4)
        # a newer readout drops the older one's timer, whose ttl would blank what is shown now
        if self._panel_reset is not None:
            self._panel_reset.cancel()
            self._panel_reset = None
        if ttl_secs > 0:
            loop = asyncio.get_running_loop()
            self._panel_reset = loop.call_later(
                ttl_secs, lambda: self.panel.show("PINCER1", "watching") if self.panel else None
            )

    def _on_body_tap(self, taps: int) -> None:
        """Two quick taps on the body are the touch on this chassis (the imu hears them); a single
        tap is only logged."""
        if taps >= 2:
            self._relay_touch(long=False, source="double tap")
        else:
            log.debug("single tap on the body")

    def _on_body_button(self, button_id: int, state: int) -> None:
        """A press of a button on the chassis' controller board is a touch too: long press as a
        long touch."""
        if state == 0:
            return
        self._relay_touch(long=state >= 3, source=f"button {button_id} state {state}")

    def _relay_touch(self, *, long: bool, source: str) -> None:
        """Until the shell has its pad, the touch reaches the daemon's touch input over http."""
        try:
            import urllib.request

            req = urllib.request.Request(
                self.cfg.daemon_http.rstrip("/") + "/dev/touch",
                data=json.dumps({"long": long}).encode(),
                headers={"content-type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=2) as r:
                r.read()
            log.info("%s relayed as a %s touch", source, "long" if long else "short")
            if self.panel is not None:
                self.panel.beep(freq=2400, on_s=0.05, repeat=1)
        except Exception as exc:
            log.warning("touch relay failed: %s", exc)

    async def climb(self, riser_m: float | None, tread_m: float = 0.28, steps: int = 1) -> str:
        """Climb the riser the floor watch sees, or the one the caller names."""
        distance = yaw = None
        if riser_m is None:
            with self.shared.lock:
                floor = self.shared.floor
            step = getattr(floor, "step", None)
            if step is None or time.time() - floor.ts > 3.0:
                return "REFUSED: no riser in view."
            riser_m, distance, yaw = step.riser_m, step.distance_m, step.yaw
            if step.tread_m is not None:
                tread_m = step.tread_m
        await self.dock.leave_dock_for("climb")
        return await self.motion.climb(
            float(riser_m), float(tread_m), int(steps), distance_m=distance, yaw_rad=yaw
        )

    async def name_room(self, name: str, radius: float | None, polygon: Any) -> dict[str, Any]:
        if name == DOCK_ROOM:
            # The robot is sitting on the dock: record it, register the staging pose.
            site = await asyncio.to_thread(self.dock.enroll, self.rear_range(max_age_s=5.0))
            r = self.nav.rooms.get(DOCK_ROOM)
            assert r is not None
            return {"name": r.name, "x": round(r.x, 2), "y": round(r.y, 2), "radius": r.radius,
                    "polygon": None, "rooms": self.nav.rooms.names(),
                    "dock": {"x": round(site.x, 2), "y": round(site.y, 2),
                             "yaw": round(site.yaw, 3), "wall_m": site.wall_m}}  # fmt: skip
        r = await asyncio.to_thread(self.nav.name_room, name, radius=radius, polygon=polygon)
        return {"name": r.name, "x": round(r.x, 2), "y": round(r.y, 2), "radius": r.radius,
                "polygon": r.polygon, "rooms": self.nav.rooms.names()}  # fmt: skip

    async def dock_action(self, action: str) -> dict[str, Any]:
        if action == "go":
            info = self.dock.go()
        elif action == "leave":
            left = await self.dock.leave_dock_for("daemon")
            info = {"ok": True, "left": left}
        else:
            info = {"ok": True}
        return {**info, **self.dock.status()}

    async def leave_dock_for(self, reason: str) -> bool:
        return await self.dock.leave_dock_for(reason)

    async def map_action(self, action: str, name: str | None) -> dict[str, Any]:
        if action == "start":
            await asyncio.to_thread(self.nav.start_mapping)
            ok = True
        elif action == "save":
            ok = (await asyncio.to_thread(self.nav.save_map, name)) is not None
        else:
            ok = True
        info = await asyncio.to_thread(self.nav.status)
        return {"ok": ok, **info}

    async def chat(self, text: str, context: dict[str, Any]) -> str:
        """Companion reply for the daemon's ``chat`` request; speaking is the service's job."""
        ctx = dict(context)
        ctx.setdefault("owner_name", self.enrol.owner_name)
        brain, fell_back = self.active_brain()
        reply = await asyncio.to_thread(brain.reply, text, ctx)
        if fell_back:
            reason = "I'm in offline mode" if self.flags.mode == "offline" else "my radio is off"
            if (
                reason != self._fallback_reason
            ):  # say it once per outage, again if the reason changes
                self._fallback_reason = reason
                reply = f"{reason}, so this is Lamport-8B talking. {reply}"
        return reply

    # -- loops ------------------------------------------------------------------------------
    def _head_pose(self) -> Any:
        """Where the depth camera is looking from: the gaze's tilt when the head is live, else the
        configured resting tilt."""
        from .terrain import CameraPose

        tilt = math.radians(self.cfg.depth_head_tilt_deg)
        yaw = 0.0
        if self.head is not None:
            tilt, yaw = self.head.gaze.tilt, self.head.gaze.pan
        return CameraPose(
            x=self.cfg.depth_head_x_m, z=self.cfg.depth_head_height_m, tilt=tilt, yaw=yaw
        )

    def _depth_thread(self, source: Any) -> None:
        from .depth import FloorWatch

        watch = FloorWatch(source, self._head_pose)
        period = 1.0 / max(0.5, self.cfg.depth_fps)
        try:
            while not self._stop.is_set():
                t0 = time.time()
                try:
                    report = watch.update(t0)
                except Exception as exc:
                    log.error("floor watch failed: %s", exc)
                    report = None
                if report is not None:
                    with self.shared.lock:
                        self.shared.floor = report
                    # the motion layer refuses a forward walk into whatever ends the floor
                    self.robot.get_status().floor_clearance_m = min(99.0, report.walkable_m)
                time.sleep(max(0.0, period - (time.time() - t0)))
        finally:
            source.close()

    def _camera_thread(self, recognizer: FaceRecognizer, source: Any) -> None:
        period = 1.0 / max(0.5, self.cfg.camera_fps)
        last_frame = time.time()
        while not self._stop.is_set():
            t0 = time.time()
            frame = source.read()
            if frame is None and t0 - last_frame > 5.0 and hasattr(source, "reopen"):
                log.warning("camera: no frames for 5 s, reopening")
                try:
                    source.reopen()
                except Exception as exc:
                    log.error("camera reopen failed: %s", exc)
                last_frame = t0
            if frame is not None:
                last_frame = t0
                try:
                    obs = recognizer.observe(frame, t0)
                except Exception as exc:
                    log.error("face recogniser failed: %s", exc)
                    obs = []
                self.on_faces(obs, t0)
            self._stop.wait(max(0.0, period - (time.time() - t0)))

    def _listen_thread(self) -> None:
        """Continuous listening for the duress word (and free passphrase utterances)."""
        verifier = self._ensure_verifier()
        while not self._stop.is_set():
            with self._mic_lock:
                audio = self.mic.record(self.cfg.listen_chunk_s)
            # a chunk recorded while the robot spoke, or that started before it stopped, is the
            # robot's own voice: never a passphrase, never a duress word (a `say` carrying the
            # words must not unlock or alarm)
            recent = time.time() - self.shared.spoke_until < self.cfg.listen_chunk_s + 1.0
            if self.shared.speaking or recent or rms(audio) < self.cfg.listen_energy_threshold:
                continue
            try:
                text = verifier.stt.transcribe(audio, self.mic.sample_rate)
            except Exception as exc:
                log.error("stt failed: %s", exc)
                continue
            self.on_transcript(text, audio)
            if self.enrol.passphrase_hashes:
                res = verifier.check(audio, self.mic.sample_rate, transcript=text)
                if res.ok:
                    with self.shared.lock:
                        self.fusion.update_voice(True, time.time())
                    self.service.ack_wake_threadsafe("passphrase")

    def _feature_extras(self, now: float) -> dict[str, Any]:
        """room_match / network / room for this tick (None = feature off)."""
        flags = self.flags
        room_known = False
        room: str | None = None
        room_match: float | None = None
        if self.nav.available:
            room_known = True
            try:
                room = self.nav.current_room()
                if flags.room_lock:
                    room_match = self.nav.room_match()  # map path; 0..1
            except Exception as exc:
                log.error("localisation failed: %s", exc)
        elif flags.room_lock and self.room.enrolled:
            with self.shared.lock:
                room_match = self.shared.room_match  # fingerprint path (lidar thread)
        network = self.airgap.network if flags.airgap != "off" else None
        b = self.battery.read(now)
        with self.shared.lock:
            self.shared.battery = b
        return dict(
            room_match=room_match,
            network=network,
            room=room,
            room_known=room_known,
            battery=(int(round(b.pct)), round(b.voltage, 2), b.charging),
        )

    async def _fusion_loop(self) -> None:
        last_extras = 0.0
        extras: dict[str, Any] = {}
        while not self._stop.is_set():
            now = time.time()
            if now - last_extras >= 0.5:
                extras = await asyncio.to_thread(self._feature_extras, now)
                last_extras = now
            try:
                self.dock.tick()
            except Exception as exc:
                log.error("dock tick failed: %s", exc)
            with self.shared.lock:
                self.fusion.set_extras(**extras, dock=self.dock.state)
                state = self.fusion.tick(now)
                if state.owner:
                    self.fusion.note_owner_seen(
                        now,
                        self.shared.last_bearing if now - self.shared.last_bearing_ts < 2 else None,
                        extras.get("room"),
                    )
                events = self.fusion.events[:]
                self.fusion.events.clear()
            for ts, ev in events:
                log.info("presence event %s @%.1f", ev, ts)
            if state.owner:
                self.airgap.owner_seen(now)
            self.airgap.evaluate(now)
            self.service.publish(state)
            await asyncio.sleep(0.1)

    async def _sway_loop(self) -> None:
        if not self.cfg.idle_sway:
            return
        while not self._stop.is_set():
            await asyncio.sleep(30.0)
            st = self.service.state
            if self.dock.state in ("charging", "approaching", "verifying"):
                continue  # keep the pogo pins where they are
            if st is not None and st.person and not st.moved:
                await self.motion.idle_sway()

    def _start_sensors(self) -> None:
        oak: Any = None  # the head camera when it serves both colour and depth
        if self.mock:
            sim = MockSensorSim(self, self.cfg.mock_scenario)
            self._threads.append(threading.Thread(target=sim.run, name="mock-sensors", daemon=True))
        else:
            if self.cfg.camera_source == "oak":
                try:
                    from .depth import OakCamera

                    oak = OakCamera(
                        self.cfg.depth_width, self.cfg.depth_height, self.cfg.camera_fps
                    )
                except Exception as exc:
                    log.error(
                        "head camera (oak) unavailable (%s): falling back to camera %d",
                        exc,
                        self.cfg.camera_index,
                    )
            try:
                from .presence.face import CameraSource, InsightFaceRecognizer

                source = (
                    oak
                    if oak is not None
                    else CameraSource(self.cfg.camera_url or self.cfg.camera_index)
                )
                rec = InsightFaceRecognizer.from_plan(self.plan, self.cfg.face_det_size)
                self._threads.append(
                    threading.Thread(
                        target=self._camera_thread, args=(rec, source), name="camera", daemon=True
                    )
                )
            except Exception as exc:
                log.error("camera/face disabled: %s", exc)
        depth_source: Any = None
        if self.mock:
            if self.cfg.mock_depth != "none":
                from .depth import MockDepthSource, mock_scene

                depth_source = MockDepthSource(
                    self._head_pose, scene=mock_scene(self.cfg.mock_depth)
                )
        elif oak is not None:
            depth_source = oak.depth  # the same device, its depth stream
        elif self.cfg.depth_enabled:
            try:
                from .depth import OakDepthSource

                depth_source = OakDepthSource(
                    self.cfg.depth_width, self.cfg.depth_height, self.cfg.depth_fps
                )
            except Exception as exc:
                log.error("depth disabled: %s", exc)
        if depth_source is not None:
            self._threads.append(
                threading.Thread(
                    target=self._depth_thread, args=(depth_source,), name="depth", daemon=True
                )
            )
        self._threads.append(threading.Thread(target=self._listen_thread, name="mic", daemon=True))
        for t in self._threads:
            t.start()

    async def run(self) -> None:
        log.info("pincer-robot %s starting (%s mode)", __version__, "mock" if self.mock else "real")
        await self.service.start()
        self._start_sensors()
        tasks = [
            asyncio.create_task(self._fusion_loop(), name="fusion"),
            asyncio.create_task(self._sway_loop(), name="sway"),
        ]
        try:
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            pass
        finally:
            self._stop.set()
            for t in tasks:
                t.cancel()
            await self.service.stop()
            await self.motion.stop()
            self.dock.close()
            self.airgap.close()
            self.nav.close()
            self.battery.close()
            shutdown = getattr(self.robot, "shutdown", None)
            if callable(shutdown):
                shutdown()


class MockSensorSim:
    """Synthetic room + face feed for laptop runs. Runs in a thread, calls the app callbacks."""

    def __init__(self, app: RobotApp, scenario: str) -> None:
        from .presence.synthetic import make_scan, person_at

        self.app = app
        self.scenario = scenario
        self._make_scan = make_scan
        self._person_at = person_at
        rng = np.random.default_rng(1)
        enrolled = app.enrol.face_matrix()
        base = enrolled[0] if len(enrolled) else rng.normal(size=512)
        self._face_emb = base if scenario == "owner" else rng.normal(size=512)
        self._rng = rng
        self._recognizer = MockFaceRecognizer()

    def run(self) -> None:
        app = self.app
        t0 = time.time()
        next_face = t0
        while not app._stop.is_set():
            now = time.time()
            t = now - t0
            objects = []
            if self.scenario in ("owner", "stranger"):
                # Walks a slow ellipse in front of the robot.
                objects.append(
                    self._person_at(1.6 + 0.5 * math.cos(t / 6.0), 0.4 * math.sin(t / 6.0))
                )
            app.on_scan(self._make_scan(now, objects, noise_m=0.01, rng=self._rng))
            if self.scenario in ("owner", "stranger") and now >= next_face:
                emb = self._face_emb + self._rng.normal(scale=0.02, size=self._face_emb.shape)
                # Slow sway plus the small jitter a real head has (the liveness gate
                # measures pose variation, a perfectly smooth sine dips below it at peaks).
                obs = make_mock_observation(
                    emb,
                    now,
                    yaw=8.0 * math.sin(t / 1.3) + self._rng.normal(scale=2.5),
                    pitch=4.0 * math.cos(t / 2.1) + self._rng.normal(scale=2.0),
                    x_norm=0.3 * math.sin(t / 6.0),
                )
                app.on_faces([obs], now)
                next_face = now + 1.0 / app.cfg.camera_fps
            elif self.scenario == "empty" and now >= next_face:
                app.on_faces([], now)
                next_face = now + 1.0 / app.cfg.camera_fps
            app.on_imu((0.0, 0.0, 9.80665), 0.0, now)
            app._stop.wait(0.1)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="pincer-robot", description="Pincer1 robot-side service")
    p.add_argument("--mock", action="store_true", help="no hardware: synthetic sensors + console")
    p.add_argument("--scenario", choices=["owner", "stranger", "empty"], help="mock room content")
    p.add_argument("--port", type=int, help="websocket port (default 7402)")
    p.add_argument("--enrol", help="enrolment file (default ~/.pincer1/enrol.json)")
    p.add_argument("--log-level", default="INFO")
    p.add_argument("--version", action="version", version=f"pincer-robot {__version__}")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    overrides: dict[str, Any] = {}
    if args.scenario:
        overrides["mock_scenario"] = args.scenario
    if args.port:
        overrides["ws_port"] = args.port
    if args.enrol:
        overrides["enrol_path"] = args.enrol
    cfg = load_settings(**overrides)
    app = RobotApp(cfg, mock=args.mock)
    try:
        asyncio.run(app.run())
    except KeyboardInterrupt:
        log.info("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
