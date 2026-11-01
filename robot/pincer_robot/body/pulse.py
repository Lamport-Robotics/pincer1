"""The body package on a chassis whose servos sit behind a ROS 2 stack: joints over rosbridge.

The first test chassis keeps its 22 bus servos behind a controller node that takes positions in
pulses (0..1000 over 240°) on a ``ServosPosition`` topic and reports them on a ``ServoStateList``
topic. ``RosbridgeServoBus`` speaks to that stack over rosbridge's websocket, so the gait, the gaze
and the climb cycle run unchanged on it: it has the same ``move`` / ``torque`` / ``position``
surface as ``servo.Bus`` and takes radians. Nothing here imports rclpy; a laptop on the same network
is enough.

``PulseCalibration`` is the radians-to-pulses map per servo (sign and zero pulse), measured once
with ``pincer-calibrate`` and kept in ``~/.pincer1/servo_calibration.json``. ``RosbridgeSensors``
subscribes to the lidar and the IMU the same way and feeds the status the motion layer reads.
"""

from __future__ import annotations

import json
import logging
import math
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

from ..motion import RobotStatus
from ..presence.lidar import Scan
from .servo import ServoError

log = logging.getLogger("pincer.pulse")

PULSE_RANGE = 1000
PULSE_DEGREES = 240.0
PULSES_PER_RAD = PULSE_RANGE / math.radians(PULSE_DEGREES)

SERVO_TOPIC = "/servo_controller"
SERVO_TYPE = "servo_controller_msgs/ServosPosition"
STATE_TOPIC = "/controller_manager/servo_states"
STATE_TYPE = "servo_controller_msgs/ServoStateList"


@dataclass
class PulseCalibration:
    """pulse = zero[id] + sign[id] * radians * PULSES_PER_RAD."""

    zero: dict[int, int] = field(default_factory=dict)
    sign: dict[int, int] = field(default_factory=dict)
    note: str = ""

    def to_pulse(self, servo_id: int, radians: float) -> int:
        p = self.zero.get(servo_id, 500) + self.sign.get(servo_id, 1) * radians * PULSES_PER_RAD
        return int(round(max(0, min(PULSE_RANGE, p))))

    def to_radians(self, servo_id: int, pulse: float) -> float:
        return (pulse - self.zero.get(servo_id, 500)) * self.sign.get(servo_id, 1) / PULSES_PER_RAD

    def complete_for(self, servo_ids: list[int]) -> bool:
        return all(sid in self.zero and sid in self.sign for sid in servo_ids)

    def to_dict(self) -> dict[str, Any]:
        return {
            "units": "pulse",
            "note": self.note,
            "servos": {
                str(sid): {"zero": self.zero[sid], "sign": self.sign.get(sid, 1)}
                for sid in sorted(self.zero)
            },
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> PulseCalibration:
        if d.get("units", "pulse") != "pulse":
            raise ValueError(f"calibration units are {d.get('units')!r}, not pulse")
        out = cls(note=str(d.get("note", "")))
        for k, v in d.get("servos", {}).items():
            out.zero[int(k)] = int(v["zero"])
            out.sign[int(k)] = 1 if int(v.get("sign", 1)) >= 0 else -1
        return out

    def save(self, path: Path) -> None:
        path = path.expanduser()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2) + "\n")

    @classmethod
    def load(cls, path: Path) -> PulseCalibration:
        return cls.from_dict(json.loads(path.expanduser().read_text()))

    @classmethod
    def chassis(cls) -> PulseCalibration:
        """The first test chassis as its controller defines it: every leg joint reads 500 at the
        description's zero (coxa along the mount, femur horizontal, tibia vertical), the right side
        counts up with a positive angle and the left side down; the arm's zero is the upright pose
        measured on the robot (shoulder 500, elbow 125, camera level at wrist 580). Run
        ``pincer-calibrate --preset chassis`` to confirm the directions on the machine in front of
        you."""
        zero: dict[int, int] = {}
        sign: dict[int, int] = {}
        knee = math.pi / 2 * PULSES_PER_RAD
        for coxa, femur, tibia in ((6, 4, 2), (12, 10, 8), (18, 16, 14)):  # right side
            zero[coxa], sign[coxa] = 500, 1
            zero[femur], sign[femur] = 500, 1
            zero[tibia], sign[tibia] = int(round(500 - knee)), 1
        for coxa, femur, tibia in (
            (5, 3, 1),
            (11, 9, 7),
            (17, 15, 13),
        ):  # left side, mirrored servos
            zero[coxa], sign[coxa] = 500, 1
            zero[femur], sign[femur] = 500, -1
            zero[tibia], sign[tibia] = int(round(500 + knee)), -1
        zero.update({19: 500, 20: 500, 21: 125, 22: 580})
        sign.update({19: 1, 20: 1, 21: 1, 22: -1})
        return cls(
            zero=zero,
            sign=sign,
            note="chassis defaults from its controller's zero; directions to confirm",
        )


class RosbridgeClient:
    """One websocket to rosbridge: advertise, publish, subscribe. Messages are rosbridge's JSON
    protocol; a background thread reads and dispatches."""

    def __init__(self, url: str, *, connect_timeout_s: float = 5.0, reconnect_s: float = 2.0):
        self.url = url
        self._timeout = connect_timeout_s
        self._reconnect_s = reconnect_s
        self._lock = threading.Lock()
        self._handlers: dict[str, Callable[[dict[str, Any]], None]] = {}
        self._advertised: dict[str, str] = {}
        self._subscribed: dict[str, dict[str, Any]] = {}
        self._alive = True
        self._connected = False
        self._ws = self._open()
        self._connected = True
        self._reader = threading.Thread(target=self._read_loop, name="rosbridge", daemon=True)
        self._reader.start()

    def _open(self):
        from websockets.sync.client import connect

        try:
            return connect(self.url, open_timeout=self._timeout, close_timeout=1, legacy=True)
        except TypeError:  # older websockets: no legacy flag, and no warning either
            return connect(self.url, open_timeout=self._timeout, close_timeout=1)

    def _reconnect(self) -> bool:
        """The chassis rebooted or the network blinked: open again, re-advertise, re-subscribe."""
        while self._alive:
            try:
                ws = self._open()
            except Exception:
                time.sleep(self._reconnect_s)
                continue
            with self._lock:
                self._ws = ws
                self._connected = True
            for topic, msg_type in list(self._advertised.items()):
                self._send({"op": "advertise", "topic": topic, "type": msg_type})
            for sub in list(self._subscribed.values()):
                self._send(sub)
            log.info("rosbridge %s back", self.url)
            return True
        return False

    def _send(self, msg: dict[str, Any]) -> None:
        data = json.dumps(msg, separators=(",", ":"))
        with self._lock:
            if not self._connected:
                return  # dropped on the floor while the link is down; state resyncs on reconnect
            try:
                self._ws.send(data)
            except Exception as exc:
                self._connected = False
                log.warning("rosbridge send failed: %s", exc)

    def advertise(self, topic: str, msg_type: str) -> None:
        self._advertised[topic] = msg_type
        self._send({"op": "advertise", "topic": topic, "type": msg_type})

    def publish(self, topic: str, msg: dict[str, Any]) -> None:
        self._send({"op": "publish", "topic": topic, "msg": msg})

    def subscribe(
        self,
        topic: str,
        msg_type: str,
        handler: Callable[[dict[str, Any]], None],
        *,
        throttle_ms: int = 0,
        queue_length: int = 1,
    ) -> None:
        self._handlers[topic] = handler
        sub = {
            "op": "subscribe",
            "topic": topic,
            "type": msg_type,
            "throttle_rate": throttle_ms,
            "queue_length": queue_length,
        }
        self._subscribed[topic] = sub
        self._send(sub)

    def _read_loop(self) -> None:
        while self._alive:
            try:
                raw = self._ws.recv()
            except Exception as exc:
                if not self._alive:
                    break
                with self._lock:
                    self._connected = False
                log.warning("rosbridge connection lost (%s); reconnecting", exc)
                if not self._reconnect():
                    break
                continue
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            if msg.get("op") == "publish":
                handler = self._handlers.get(msg.get("topic", ""))
                if handler is not None:
                    try:
                        handler(msg.get("msg", {}))
                    except Exception as exc:  # a bad frame must not kill the reader
                        log.warning("rosbridge handler %s: %s", msg.get("topic"), exc)

    @property
    def alive(self) -> bool:
        return self._alive and self._connected

    def close(self) -> None:
        self._alive = False
        self._connected = False
        try:
            self._ws.close()
        except Exception:
            pass


class RosbridgeServoBus:
    """``servo.Bus`` for the chassis behind rosbridge: ``move`` publishes one ``ServosPosition`` in
    pulses, ``position`` answers from the last state message. Torque cannot be switched on that
    stack, so ``torque`` only logs; temperature and load are not reported, so health polling is off
    on this backend."""

    def __init__(
        self,
        client: RosbridgeClient,
        calibration: PulseCalibration,
        *,
        state_timeout_s: float = 1.0,
    ):
        self.client = client
        self.cal = calibration
        self.state_timeout_s = state_timeout_s
        self._positions: dict[int, float] = {}
        self._state_ts = 0.0
        self._torque_said = False
        client.advertise(SERVO_TOPIC, SERVO_TYPE)
        client.subscribe(STATE_TOPIC, STATE_TYPE, self._on_state, throttle_ms=50)

    def _on_state(self, msg: dict[str, Any]) -> None:
        for s in msg.get("servo_state", []):
            self._positions[int(s["id"])] = float(s["position"])
        self._state_ts = time.monotonic()

    # --- the Bus surface -------------------------------------------------------------------
    def move(self, targets: dict[int, float], time_ms: int = 0, speed: int = 0) -> None:
        rows = [
            {"id": sid, "position": float(self.cal.to_pulse(sid, rad))}
            for sid, rad in sorted(targets.items())
        ]
        self.client.publish(
            SERVO_TOPIC,
            {"duration": max(0.02, time_ms / 1000.0), "position_unit": "pulse", "position": rows},
        )

    def torque(self, servo_ids: list[int], on: bool) -> None:
        if not self._torque_said:
            log.info("joint backend: torque is managed by the chassis controller, request ignored")
            self._torque_said = True

    def ping(self, servo_id: int) -> bool:
        return servo_id in self._positions and self.state_fresh()

    def state_fresh(self) -> bool:
        return time.monotonic() - self._state_ts < self.state_timeout_s

    def pulse(self, servo_id: int) -> float:
        if servo_id not in self._positions or not self.state_fresh():
            raise ServoError(f"servo {servo_id}: no fresh state from the chassis")
        return self._positions[servo_id]

    def pulses(self) -> dict[int, float]:
        return dict(self._positions)

    def position(self, servo_id: int) -> float:
        return self.cal.to_radians(servo_id, self.pulse(servo_id))

    def temperature(self, servo_id: int) -> int:
        raise ServoError("the chassis controller does not report servo temperature")

    def voltage(self, servo_id: int) -> float:
        raise ServoError("the chassis controller does not report servo voltage")

    def load(self, servo_id: int) -> int:
        raise ServoError("the chassis controller does not report servo load")

    def wait_for_state(self, timeout_s: float = 3.0) -> bool:
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            if self._positions:
                return True
            time.sleep(0.05)
        return False


class TapDetector:
    """A tap on the body is a short spike in the accelerometer while the body is still. Two taps
    within ``double_s`` count as one confirmation (fires 2), a lone tap after the window fires 1.
    Walking is excluded by the caller; a spike has to rise above the running mean by
    ``threshold``."""

    def __init__(
        self, threshold_mps2: float = 3.0, refractory_s: float = 0.25, double_s: float = 0.7
    ):
        self.threshold, self.refractory, self.double = threshold_mps2, refractory_s, double_s
        self._mean: float | None = None
        self._last_tap = -1e9
        self._pending_single: float | None = None

    def update(self, accel: tuple[float, float, float], now: float) -> int:
        mag = math.sqrt(sum(v * v for v in accel))
        if self._mean is None:
            self._mean = mag
        spike = mag - self._mean > self.threshold
        self._mean = 0.9 * self._mean + 0.1 * mag if not spike else self._mean
        fired = 0
        if spike and now - self._last_tap > self.refractory:
            if self._pending_single is not None and now - self._pending_single <= self.double:
                self._pending_single = None
                fired = 2
            else:
                self._pending_single = now
            self._last_tap = now
        elif self._pending_single is not None and now - self._pending_single > self.double:
            self._pending_single = None
            fired = 1
        return fired


def quaternion_tilt(x: float, y: float, z: float, w: float) -> float:
    """Tilt in degrees, the larger of roll and pitch."""
    sinr_cosp = 2 * (w * x + y * z)
    cosr_cosp = 1 - 2 * (x * x + y * y)
    roll = math.atan2(sinr_cosp, cosr_cosp)
    sinp = 2 * (w * y - z * x)
    pitch = math.asin(max(-1.0, min(1.0, sinp)))
    return math.degrees(max(abs(roll), abs(pitch)))


def scan_from_json(msg: dict[str, Any], ts: float) -> Scan:
    ranges = np.array(
        [float("nan") if r is None else float(r) for r in msg.get("ranges", [])], dtype=np.float64
    )
    return Scan(
        angle_min=float(msg.get("angle_min", -math.pi)),
        angle_increment=float(msg.get("angle_increment", 0.0)),
        ranges=ranges,
        ts=ts,
        range_min=float(msg.get("range_min", 0.05)),
        range_max=float(msg.get("range_max", 12.0)),
    )


class RosbridgeSensors:
    """Lidar and IMU over the same websocket: writes tilt and front clearance into the shared status
    and hands the scan / imu to the app's callbacks, like ``ros_backend.SensorNode`` does with
    rclpy."""

    def __init__(
        self,
        client: RosbridgeClient,
        status: RobotStatus,
        *,
        scan_topic: str = "/scan",
        imu_topic: str = "/imu",
        on_scan: Callable[[Scan], None] | None = None,
        on_imu: Callable[[tuple[float, float, float], float, float], None] | None = None,
        on_tap: Callable[[int], None] | None = None,
    ):
        from ..ros_backend import front_clearance

        self._front = front_clearance
        self.status = status
        self._on_scan, self._on_imu = on_scan, on_imu
        self._on_tap = on_tap
        self.taps = TapDetector()
        client.subscribe(scan_topic, "sensor_msgs/LaserScan", self._scan, throttle_ms=100)
        client.subscribe(imu_topic, "sensor_msgs/Imu", self._imu, throttle_ms=100)

    def _scan(self, msg: dict[str, Any]) -> None:
        scan = scan_from_json(msg, time.time())
        clearance = self._front(scan)
        if clearance is not None:
            self.status.front_clearance_m = clearance
        if self._on_scan is not None:
            self._on_scan(scan)

    def _imu(self, msg: dict[str, Any]) -> None:
        q = msg.get("orientation", {})
        tilt = quaternion_tilt(
            float(q.get("x", 0)), float(q.get("y", 0)), float(q.get("z", 0)), float(q.get("w", 1))
        )
        self.status.tilt_deg = tilt
        a = msg.get("linear_acceleration", {})
        if self._on_tap is not None and not self.status.is_moving:
            n = self.taps.update(
                (float(a.get("x", 0)), float(a.get("y", 0)), float(a.get("z", 0))), time.monotonic()
            )
            if n:
                self._on_tap(n)
        if self._on_imu is not None:
            self._on_imu(
                (float(a.get("x", 0)), float(a.get("y", 0)), float(a.get("z", 0))),
                tilt,
                time.time(),
            )

    def shutdown(self) -> None:
        pass


OLED_TOPIC = "/ros_robot_controller/set_oled"
BUZZER_TOPIC = "/ros_robot_controller/set_buzzer"
BUTTON_TOPIC = "/ros_robot_controller/button"


class RosbridgePanel:
    """The chassis' own front panel over the same socket: a two-line OLED (16 characters a line),
    a buzzer, and the two buttons on its controller board. On this chassis the button is the touch
    and the OLED is the readout; the robot's shell will carry a pad and a bigger display."""

    def __init__(
        self, client: RosbridgeClient, *, on_button: Callable[[int, int], None] | None = None
    ):
        self.client = client
        self._on_button = on_button
        client.advertise(OLED_TOPIC, "ros_robot_controller_msgs/OLEDState")
        client.advertise(BUZZER_TOPIC, "ros_robot_controller_msgs/BuzzerState")
        client.subscribe(BUTTON_TOPIC, "ros_robot_controller_msgs/ButtonState", self._button)

    def show(self, line1: str, line2: str = "") -> None:
        for index, text in ((1, line1), (2, line2)):
            self.client.publish(OLED_TOPIC, {"index": index, "text": text[:16]})

    def beep(
        self, freq: int = 1800, on_s: float = 0.08, off_s: float = 0.08, repeat: int = 1
    ) -> None:
        self.client.publish(
            BUZZER_TOPIC,
            {
                "freq": int(freq),
                "on_time": float(on_s),
                "off_time": float(off_s),
                "repeat": int(repeat),
            },
        )

    def _button(self, msg: dict[str, Any]) -> None:
        if self._on_button is not None:
            self._on_button(int(msg.get("id", 0)), int(msg.get("state", 0)))


CMD_VEL_TOPIC = "/controller/cmd_vel"


class RosbridgeDrive:
    """Locomotion by the chassis' own gait controller: a Twist on its cmd_vel topic for the
    duration,
    then zero. The body package still owns the arm and the head; the legs walk the way the chassis
    already knows how to. ``max_vx`` / ``max_wz`` are the controller's clamps."""

    def __init__(
        self,
        client: RosbridgeClient,
        *,
        topic: str = CMD_VEL_TOPIC,
        max_vx: float = 0.12,
        max_wz: float = 0.6,
        rate_hz: float = 10.0,
        sleep: Callable[[float], None] = time.sleep,
    ):
        self.client, self.topic = client, topic
        self.max_vx, self.max_wz, self.rate_hz, self._sleep = max_vx, max_wz, rate_hz, sleep
        client.advertise(topic, "geometry_msgs/Twist")

    def twist(self, vx: float, wz: float) -> None:
        self.client.publish(
            self.topic,
            {
                "linear": {"x": float(vx), "y": 0.0, "z": 0.0},
                "angular": {"x": 0.0, "y": 0.0, "z": float(wz)},
            },
        )

    def drive(self, lin_x: float, ang_z: float, duration_s: float) -> None:
        """``lin_x`` / ``ang_z`` are the motion layer's -1..1; held for ``duration_s`` at
        ``rate_hz``."""
        vx = max(-1.0, min(1.0, lin_x)) * self.max_vx
        wz = max(-1.0, min(1.0, ang_z)) * self.max_wz
        end = time.monotonic() + duration_s
        period = 1.0 / self.rate_hz
        while time.monotonic() < end:
            self.twist(vx, wz)
            self._sleep(period)
        self.stop()

    def stop(self) -> None:
        self.twist(0.0, 0.0)
