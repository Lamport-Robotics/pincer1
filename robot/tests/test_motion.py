import asyncio
import math

from pincer_robot.motion import ATTENTION_STEP_M, MockRobot, MotionController


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
    assert abs(dur - ATTENTION_STEP_M / (0.15 * 0.5)) < 1e-6


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
