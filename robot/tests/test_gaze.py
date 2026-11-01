import math
import struct

import numpy as np

from pincer_robot.body import (
    ArmPose,
    Bus,
    Gaze,
    GazeConfig,
    GazeHead,
    LoopbackTransport,
    largest_face,
)
from pincer_robot.body import servo as sv
from pincer_robot.presence.face import make_mock_observation

HFOV = math.radians(62.0)
VFOV = math.radians(48.0)


def camera(face_pan: float, face_tilt: float, g: Gaze) -> tuple[float, float] | None:
    """Where a face at a fixed bearing lands in the image for the current head pose, or None
    when it is outside the field of view."""
    x = (g.pan - face_pan) / (HFOV / 2)  # face left of the optical axis -> negative x
    y = (g.tilt - face_tilt) / (VFOV / 2)  # face above the axis -> negative y
    if abs(x) > 1 or abs(y) > 1:
        return None
    return (x, y)


def test_face_on_the_right_turns_the_head_right_and_converges():
    g = Gaze(GazeConfig(), now=0.0)
    bearing, elevation = math.radians(-20), math.radians(10)  # right of centre, a bit up
    t = 0.0
    for _ in range(40):
        t += 0.1
        g.update(camera(bearing, elevation, g), t)
    assert g.mode == "tracking"
    assert abs(g.pan - bearing) < math.radians(1.5)
    assert abs(g.tilt - elevation) < math.radians(1.5)
    assert g.pan < 0  # turned right


def test_deadband_leaves_a_centred_face_alone():
    g = Gaze(GazeConfig(deadband=0.05), now=0.0)
    g.update((0.03, -0.02), 0.1)
    assert g.pan == 0.0 and g.tilt == 0.0


def test_rate_limit_and_joint_limits():
    cfg = GazeConfig(max_rate_rad_s=1.0, pan_limits=(-0.3, 0.3))
    g = Gaze(cfg, now=0.0)
    g.update((1.0, 0.0), 0.1)  # far right: wants ~0.38 rad in one go
    assert math.isclose(g.pan, -0.1, abs_tol=1e-9)  # but moves at most 1 rad/s * 0.1 s
    for i in range(2, 20):
        g.update((1.0, 0.0), i * 0.1)
    assert g.pan == -0.3  # and never past the limit


def test_hold_then_scan_when_the_face_is_lost():
    cfg = GazeConfig(hold_s=1.0, scan_after_s=3.0, scan_period_s=4.0, max_rate_rad_s=10.0)
    g = Gaze(cfg, now=0.0)
    for i in range(1, 11):
        g.update((-0.5, 0.0), i * 0.1)  # face on the left for a second
    pan_seen = g.pan
    assert pan_seen > 0
    g.update(None, 1.5)
    assert g.mode == "hold" and g.pan == pan_seen
    g.update(None, 2.5)  # past hold: heading back to centre, scan not sweeping yet
    assert g.mode == "scan" and abs(g.pan) < 1e-9 and math.isclose(g.tilt, cfg.scan_tilt)
    pans = []
    for i in range(0, 41):
        pans.append(g.update(None, 4.0 + i * 0.1).pan)
    assert max(pans) > cfg.scan_amplitude * 0.9 and min(pans) < -cfg.scan_amplitude * 0.9
    assert all(abs(p) <= cfg.scan_amplitude + 1e-9 for p in pans)


def test_servo_targets_follow_the_arm_frames():
    g = Gaze(GazeConfig(rest=ArmPose(shoulder=0.1, elbow=-0.2)), now=0.0)
    g.pan, g.tilt = 0.4, 0.25
    assert g.head_targets() == {19: 0.4, 22: -0.25}  # camera up = negative wrist pitch
    arm = g.arm_targets()
    assert set(arm) == {19, 20, 21, 22}
    assert arm[20] == 0.1 and arm[21] == -0.2


def test_largest_face_wins_and_carries_both_axes():
    e = np.ones(8)
    small = make_mock_observation(e, 0.0, x_norm=0.8, y_norm=0.0)
    big = make_mock_observation(e, 0.0, x_norm=-0.4, y_norm=0.3)
    big = type(big)(big.embedding, (0, 0, 300, 300), big.pose, 640, 0.0, 1.0, 480)
    x, y = largest_face([small, big])
    assert math.isclose(x, 150 / 320 - 1) and math.isclose(y, 150 / 240 - 1)
    assert largest_face([]) is None


def goal_ticks(frame: bytes) -> dict[int, int]:
    # FF FF FE len 83 addr width (id lo hi lo hi lo hi)* chk
    assert frame[:5] == b"\xff\xff\xfe" + bytes([frame[3]]) + b"\x83"
    width = frame[6]
    rows = frame[7:-1]
    out = {}
    for i in range(0, len(rows), width + 1):
        out[rows[i]] = struct.unpack("<H", rows[i + 1 : i + 3])[0]
    return out


def test_head_writes_the_bus_only_when_the_target_moves():
    io = LoopbackTransport()
    head = GazeHead(Bus(io), Gaze(GazeConfig(), now=0.0))
    head.raise_head()
    assert len(io.frames) == 2  # torque on, then the arm pose
    arm = goal_ticks(io.frames[1])
    assert set(arm) == {19, 20, 21, 22} and all(v == sv.CENTRE for v in arm.values())
    e = np.ones(8)
    head.observe([make_mock_observation(e, 0.1, x_norm=0.6)], 0.1)
    assert len(io.frames) == 3
    pan = goal_ticks(io.frames[2])[19]
    assert pan < sv.CENTRE  # turned right
    head.observe([], 0.2)  # lost: hold, no new packet
    assert len(io.frames) == 3
