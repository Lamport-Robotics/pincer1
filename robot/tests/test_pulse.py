import json
import math
import threading
import time

import numpy as np
import pytest

from pincer_robot.body.gaze import GazeConfig
from pincer_robot.body.geometry import BodyGeometry
from pincer_robot.body.pulse import (
    PULSES_PER_RAD,
    STATE_TOPIC,
    PulseCalibration,
    RosbridgeClient,
    RosbridgeServoBus,
    quaternion_tilt,
    scan_from_json,
)
from pincer_robot.body.servo import ServoError
from pincer_robot.calibrate import Joint, Session, jog_step, joints, solve_zero


def test_pulse_calibration_round_trip_and_clamp(tmp_path):
    cal = PulseCalibration(zero={1: 500, 2: 300}, sign={1: 1, 2: -1}, note="bench")
    assert cal.to_pulse(1, 0.0) == 500
    assert cal.to_pulse(1, math.radians(24)) == 600  # 100 pulses per 24 degrees
    assert cal.to_pulse(2, math.radians(24)) == 200
    assert cal.to_pulse(1, 10.0) == 1000 and cal.to_pulse(2, 10.0) == 0
    assert math.isclose(cal.to_radians(2, 200), math.radians(24), rel_tol=1e-9)
    path = tmp_path / "cal.json"
    cal.save(path)
    back = PulseCalibration.load(path)
    assert back.zero == cal.zero and back.sign == cal.sign and back.note == "bench"
    assert back.complete_for([1, 2]) and not back.complete_for([1, 3])
    with pytest.raises(ValueError):
        PulseCalibration.from_dict({"units": "tick", "servos": {}})


def test_solve_zero_uses_the_neutral_angle_and_sign():
    # the knee sits at 90 degrees in the neutral pose; a servo reading 700 there with sign +1 has
    # its kinematic zero 375 pulses lower
    assert solve_zero(700, math.pi / 2, 1) == 700 - round(math.pi / 2 * PULSES_PER_RAD)
    assert solve_zero(700, math.pi / 2, -1) == 700 + round(math.pi / 2 * PULSES_PER_RAD)
    assert solve_zero(512, 0.0, -1) == 512


def test_joint_list_covers_legs_and_arm():
    js = joints(BodyGeometry(), GazeConfig())
    assert len(js) == 22 and [j.servo_id for j in js[:3]] == [1, 2, 3]
    assert js[-1].servo_id == 22 and js[2].neutral_rad == math.pi / 2
    assert (
        jog_step("a") == -10
        and jog_step("W") == 2
        and jog_step("") is None
        and jog_step("x") is None
    )


class FakeRosbridge:
    """A websocket server that records what the client sends and can publish state."""

    def __init__(self):
        from websockets.sync.server import serve

        self.received: list[dict] = []
        self._conn = None
        self._server = serve(self._handle, "127.0.0.1", 0)
        self.port = self._server.socket.getsockname()[1]
        threading.Thread(target=self._server.serve_forever, daemon=True).start()

    def _handle(self, conn):
        self._conn = conn
        try:
            for raw in conn:
                self.received.append(json.loads(raw))
        except Exception:
            pass

    def push(self, topic: str, msg: dict) -> None:
        for _ in range(50):
            if self._conn is not None:
                break
            time.sleep(0.02)
        self._conn.send(json.dumps({"op": "publish", "topic": topic, "msg": msg}))

    def wait_for(self, op: str, n: int = 1, timeout: float = 2.0) -> list[dict]:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            got = [m for m in self.received if m.get("op") == op]
            if len(got) >= n:
                return got
            time.sleep(0.02)
        return [m for m in self.received if m.get("op") == op]

    def close(self):
        self._server.shutdown()


def test_rosbridge_bus_publishes_pulses_and_reads_state():
    srv = FakeRosbridge()
    try:
        client = RosbridgeClient(f"ws://127.0.0.1:{srv.port}")
        cal = PulseCalibration(zero={1: 500, 19: 500, 22: 580}, sign={1: 1, 19: 1, 22: -1})
        bus = RosbridgeServoBus(client, cal)
        adv = srv.wait_for("advertise")
        assert adv and adv[0]["topic"] == "/servo_controller"
        subs = srv.wait_for("subscribe")
        assert any(s["topic"] == STATE_TOPIC for s in subs)
        with pytest.raises(ServoError):
            bus.pulse(1)
        srv.push(
            STATE_TOPIC, {"servo_state": [{"id": 1, "position": 600}, {"id": 22, "position": 580}]}
        )
        assert bus.wait_for_state(2.0)
        assert bus.pulse(1) == 600 and math.isclose(bus.position(1), math.radians(24), rel_tol=1e-6)
        assert bus.position(22) == 0.0 and bus.ping(22) and not bus.ping(19)
        bus.move({19: math.radians(24), 22: 0.1}, time_ms=120)
        pub = srv.wait_for("publish")
        assert pub and pub[0]["topic"] == "/servo_controller"
        m = pub[0]["msg"]
        assert m["position_unit"] == "pulse" and m["duration"] == pytest.approx(0.12)
        rows = {r["id"]: r["position"] for r in m["position"]}
        assert rows[19] == 600.0 and rows[22] == float(580 - round(0.1 * PULSES_PER_RAD))
        bus.torque([1, 2], True)  # logged once, nothing on the wire
        time.sleep(0.1)
        assert len([x for x in srv.received if x.get("op") == "publish"]) == 1
        with pytest.raises(ServoError):
            bus.temperature(1)
        client.close()
    finally:
        srv.close()


def test_calibration_session_records_neutral_and_sign():
    srv = FakeRosbridge()
    try:
        client = RosbridgeClient(f"ws://127.0.0.1:{srv.port}")
        bus = RosbridgeServoBus(client, PulseCalibration())
        srv.wait_for("subscribe")
        srv.push(STATE_TOPIC, {"servo_state": [{"id": 3, "position": 700}]})
        assert bus.wait_for_state(2.0)
        answers = iter(["d", "d", "", "", "n"])  # jog +20, accept, enter to nudge, then "wrong way"
        said: list[str] = []
        s = Session(bus, PulseCalibration(), ask=lambda _p="": next(answers), say=said.append)
        s._set = lambda sid, pulse, seconds=0.0: s.bus.client.publish(  # no sleeping in tests
            "/servo_controller", {"position": [{"id": sid, "position": pulse}]}
        )
        tibia = Joint(3, "FR tibia", math.pi / 2, "tibia vertical", "foot lifts")
        cal = s.run([tibia], do_jog=True, do_signs=True)
        assert s.neutral_pulse[3] == 720 and cal.sign[3] == -1
        assert cal.zero[3] == solve_zero(720, math.pi / 2, -1)
        pubs = srv.wait_for("publish", 4)
        assert [r["msg"]["position"][0]["position"] for r in pubs[:2]] == [710, 720]  # the jogs
        client.close()
    finally:
        srv.close()


def test_sensor_helpers():
    assert quaternion_tilt(0, 0, 0, 1) == 0.0
    assert math.isclose(
        quaternion_tilt(math.sin(0.1), 0, 0, math.cos(0.1)), math.degrees(0.2), rel_tol=1e-6
    )
    scan = scan_from_json(
        {"angle_min": -1.0, "angle_increment": 0.5, "ranges": [1.0, None, 3.0]}, 5.0
    )
    assert scan.ts == 5.0 and np.isnan(scan.ranges[1]) and scan.ranges[2] == 3.0


def test_rosbridge_drive_holds_a_twist_then_stops():
    from pincer_robot.body.pulse import RosbridgeDrive

    srv = FakeRosbridge()
    try:
        client = RosbridgeClient(f"ws://127.0.0.1:{srv.port}")
        d = RosbridgeDrive(client, rate_hz=1000.0, sleep=lambda _s: None)
        adv = srv.wait_for("advertise")
        assert adv and adv[0]["topic"] == "/controller/cmd_vel"
        first = srv.wait_for("publish", 2)  # a stop goes out as soon as the topics are advertised
        assert first[0]["msg"]["linear"]["x"] == 0.0 and first[1]["msg"]["gait"] == 0
        d.drive(0.5, -1.0, 0.01)
        time.sleep(0.2)
        twists = [
            m["msg"]
            for m in srv.received
            if m.get("op") == "publish" and m["topic"] == "/controller/cmd_vel"
        ]
        assert twists[1]["linear"]["x"] == pytest.approx(0.06)
        assert twists[1]["angular"]["z"] == pytest.approx(-0.6)
        assert twists[-1]["linear"]["x"] == 0.0 and twists[-1]["angular"]["z"] == 0.0
        stops = [
            m["msg"]
            for m in srv.received
            if m.get("op") == "publish" and m["topic"] == "/controller/traveling"
        ]
        assert len(stops) >= 2 and all(s["gait"] == 0 for s in stops)
        client.close()
    finally:
        srv.close()
