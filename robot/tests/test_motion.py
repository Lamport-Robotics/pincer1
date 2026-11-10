import asyncio
import math

from pincer_robot.motion import ATTENTION_STEP_M, GAIT_SPEED_MPS, MockRobot, MotionController


def test_attention_turns_toward_bearing_then_steps() -> None:
    robot = MockRobot()
    mc = MotionController(robot)
    asyncio.run(mc.attention(math.radians(40)))
    assert len(robot.log) == 2
    lin, ang, dur = robot.log[0]
    assert lin == 0.0 and ang > 0  # left turn
    assert abs(dur - 40 / (45.0 * 0.5)) < 1e-6
    lin, ang, dur = robot.log[1]
    assert lin > 0 and ang == 0.0
    assert abs(dur - ATTENTION_STEP_M / (GAIT_SPEED_MPS * 0.5)) < 1e-6


def test_attention_without_bearing_or_small_bearing_only_steps() -> None:
    robot = MockRobot()
    mc = MotionController(robot)
    asyncio.run(mc.attention(None))
    asyncio.run(mc.attention(math.radians(2)))
    assert all(ang == 0.0 for _, ang, _ in robot.log)
    assert len(robot.log) == 2


def test_safety_refusals() -> None:
    robot = MockRobot()
    robot.get_status().front_clearance_m = 0.1
    assert robot.walk("forward", 0.1).startswith("REFUSED")
    robot.get_status().front_clearance_m = 9.0
    robot.get_status().tilt_deg = 40.0
    assert robot.walk("forward", 0.1).startswith("REFUSED")
    assert robot.turn("up", 10.0).startswith("REFUSED")
    robot.get_status().tilt_deg = 0.0
    assert robot.walk("forward", 99.0).startswith("OK")
    assert robot.log[-1][2] <= 2.0 / (0.15 * 0.5) + 1e-9  # clamped to 2 m


def test_idle_sway_is_small_and_symmetric() -> None:
    robot = MockRobot()
    asyncio.run(MotionController(robot).idle_sway())
    net = sum(ang * dur for _, ang, dur in robot.log)
    assert abs(net) < 1e-9


def test_the_floor_ahead_refuses_a_forward_walk_too() -> None:
    robot = MockRobot()
    robot.get_status().floor_clearance_m = 0.2
    assert robot.walk("forward", 0.5).startswith("REFUSED: the floor ahead")
    assert robot.walk("backward", 0.2).startswith("OK")
    robot.get_status().floor_clearance_m = 9.0
    assert robot.walk("forward", 0.2).startswith("OK")


def test_climb_is_refused_by_a_body_without_legs() -> None:
    robot = MockRobot()
    assert robot.climb(0.12).startswith("REFUSED")
    ctl = MotionController(robot)
    assert asyncio.run(ctl.climb(0.12, 0.28, 1)).startswith("REFUSED")


def test_front_clearance_for_both_scan_conventions() -> None:
    """An obstacle 10 degrees right of straight ahead is seen whether the lidar publishes its
    beams over -pi..pi or over 0..2pi (where it sits near 2pi)."""
    import numpy as np

    from pincer_robot.presence.lidar import Scan
    from pincer_robot.ros_backend import front_clearance

    n = 360
    inc = 2 * math.pi / n
    for angle_min in (-math.pi, 0.0):
        angles = angle_min + inc * np.arange(n)
        ranges = np.full(n, 3.0)
        off = (angles - math.radians(-10) + math.pi) % (2 * math.pi) - math.pi
        ranges[np.abs(off) < inc / 2] = 0.2
        scan = Scan(angle_min=angle_min, angle_increment=inc, ranges=ranges, ts=0.0)
        assert front_clearance(scan) == 0.2
