import math
import struct

import pytest

from pincer_robot.body import (
    GAITS,
    Body,
    BodyBackend,
    BodyGeometry,
    BodyPose,
    Bus,
    BusActuator,
    Calibration,
    GaitParams,
    JointAngles,
    LoopbackTransport,
    ReachError,
    Simulation,
    Velocity,
    forward,
    inverse,
    simulated_body,
    solve_leg,
)
from pincer_robot.body import servo as sv
from pincer_robot.body.gait import GaitState, feet_down, limit_command, step, stride_vectors
from pincer_robot.body.kinematics import (
    body_to_leg,
    foot_in_body_frame,
    leg_to_body,
    rotate,
    rotate_inverse,
)

G = BodyGeometry()


# --- kinematics ---------------------------------------------------------------------------------
def test_inverse_then_forward_round_trip():
    for x, y, z in [
        (0.14, 0.0, -0.09),
        (0.12, 0.05, -0.06),
        (0.10, -0.04, -0.12),
        (0.20, 0.0, -0.02),
    ]:
        a = inverse(G, (x, y, z))
        fx, fy, fz = forward(G, a)
        assert (
            math.isclose(fx, x, abs_tol=1e-9)
            and math.isclose(fy, y, abs_tol=1e-9)
            and math.isclose(fz, z, abs_tol=1e-9)
        )


def test_straight_leg_is_the_reach_limit():
    a = inverse(G, (G.reach_m - 1e-6, 0.0, 0.0), check_limits=False)
    assert math.isclose(
        a.tibia, math.pi, abs_tol=2e-2
    )  # acos is flat at 1: a micron short is a few millirad
    with pytest.raises(ReachError):
        inverse(G, (G.reach_m + 0.01, 0.0, 0.0))
    with pytest.raises(ReachError):
        inverse(G, (0.01, 0.0, -0.05))  # inside the coxa radius


def test_joint_limits_are_enforced():
    with pytest.raises(ReachError, match="coxa"):
        inverse(G, (0.05, 0.12, -0.09))  # 67 deg of coxa yaw


def test_frames_round_trip_and_mount_yaw():
    for leg in range(6):
        p = (0.03, -0.02, -0.08)
        assert all(
            math.isclose(a, b, abs_tol=1e-12)
            for a, b in zip(body_to_leg(G, leg, leg_to_body(G, leg, p)), p, strict=True)
        )
    # the neutral foot of the mid-right leg points straight right
    nx, ny, _ = G.neutral_foot(1)
    assert math.isclose(nx, 0.0, abs_tol=1e-9) and ny < -0.2


def test_rotation_and_its_inverse():
    v = (0.1, -0.05, 0.02)
    r = rotate(v, 0.2, -0.1, 0.3)
    back = rotate_inverse(r, 0.2, -0.1, 0.3)
    assert all(math.isclose(a, b, abs_tol=1e-12) for a, b in zip(back, v, strict=True))


def test_body_pose_keeps_feet_planted():
    # raise the body 2 cm: every foot ends up 2 cm lower in the body frame, nowhere else
    foot = G.neutral_foot(0)
    moved = foot_in_body_frame(G, 0, foot, BodyPose(z=0.02))
    assert (
        math.isclose(moved[2], foot[2] - 0.02, abs_tol=1e-12)
        and math.isclose(moved[0], foot[0])
        and math.isclose(moved[1], foot[1])
    )
    # a pitch forward lowers the front feet and raises the rear ones in the body frame
    front = foot_in_body_frame(G, 0, G.neutral_foot(0), BodyPose(pitch=math.radians(5)))
    rear = foot_in_body_frame(G, 2, G.neutral_foot(2), BodyPose(pitch=math.radians(5)))
    assert front[2] > G.neutral_foot(0)[2] and rear[2] < G.neutral_foot(2)[2]


def test_neutral_stance_is_reachable_for_every_leg():
    for leg in range(6):
        a = solve_leg(G, leg, G.neutral_foot(leg))
        assert math.isclose(a.coxa, 0.0, abs_tol=1e-9)  # neutral feet sit on the mount yaw


# --- gait -----------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", list(GAITS))
def test_each_gait_keeps_enough_feet_down_through_a_cycle(name):
    params = GaitParams(name=name, cycle_s=1.0)
    state = GaitState()
    minimum = {"tripod": 3, "ripple": 4, "wave": 5}[name]
    for _ in range(200):
        step(G, state, Velocity(vx=0.05), params, 0.005)
        assert sum(feet_down(state, params)) >= minimum


def test_tripod_alternates_its_two_groups():
    params = GaitParams(name="tripod")
    state = GaitState(phase=0.25, stopped=False)
    down = feet_down(state, params)
    assert down == [True, False, True, False, True, False]
    state.phase = 0.75
    assert feet_down(state, params) == [False, True, False, True, False, True]


def test_stride_is_speed_times_stance_time_and_rotation_is_tangent():
    params = GaitParams(name="tripod", cycle_s=1.0)
    sx, sy = stride_vectors(G, Velocity(vx=0.1), params)[1]
    assert math.isclose(sx, 0.05) and math.isclose(sy, 0.0)
    strides = stride_vectors(G, Velocity(wz=1.0), params)
    fr, fl = strides[0], strides[5]
    # turning left: the tangent of +wz at a foot on the right (y<0) points +x
    assert fr[0] > 0 and fl[0] < 0


def test_command_is_scaled_to_the_step_length():
    params = GaitParams(name="tripod", cycle_s=1.0, step_length_m=0.04)
    v = limit_command(G, Velocity(vx=1.0), params)
    assert math.isclose(v.vx, 0.08)  # 0.04 m per 0.5 s of stance
    assert limit_command(G, Velocity(vx=0.05), params).vx == 0.05


def test_swing_lifts_and_stance_stays_on_the_ground():
    params = GaitParams(name="tripod", cycle_s=1.0, step_height_m=0.03)
    state = GaitState()
    lifted = 0
    for _ in range(100):
        feet = step(G, state, Velocity(vx=0.05), params, 0.01)
        for leg, down in enumerate(feet_down(state, params)):
            z = feet[leg][2] - G.neutral_foot(leg)[2]
            if down:
                assert abs(z) < 1e-9
            else:
                assert z >= -1e-9 and z <= 0.03 + 1e-9
                lifted += 1
    assert lifted > 0


def test_stopping_finishes_the_step_then_holds():
    params = GaitParams(name="tripod", cycle_s=1.0)
    state = GaitState()
    for _ in range(30):
        step(G, state, Velocity(vx=0.05), params, 0.01)
    assert not all(feet_down(state, params))
    for _ in range(100):
        feet = step(G, state, Velocity(), params, 0.01)
    assert all(feet_down(state, params))
    assert all(math.isclose(f[2], G.neutral_foot(i)[2]) for i, f in enumerate(feet))
    phase = state.phase
    step(G, state, Velocity(), params, 0.01)
    assert state.phase == phase  # holding


# --- simulation / stability -----------------------------------------------------------------------
@pytest.mark.parametrize("name", list(GAITS))
def test_simulated_walk_stays_statically_stable(name):
    body, sim = simulated_body(gait=GaitParams(name=name, cycle_s=1.0, step_length_m=0.05))
    body.stand(actuate=True)
    body.set_velocity(0.06, 0.02, 0.3)
    worst = math.inf
    for _ in range(400):
        body.tick(0.005)
        r = sim.report()
        assert r.stable, (name, r)
        worst = min(worst, r.support_margin_m)
    assert worst > 0.01  # a centimetre of margin at the worst moment


def test_simulation_rejects_angles_outside_the_limits():
    sim = Simulation(G)
    bad = [JointAngles(0.0, 0.0, math.radians(10))] * 6
    with pytest.raises(ValueError, match="tibia"):
        sim.apply(bad)


def test_sit_and_stand_change_the_height():
    body, sim = simulated_body()
    body.stand(actuate=True)
    standing = sim.feet()[0][2]
    body.sit(actuate=True)
    sitting = sim.feet()[0][2]
    assert math.isclose(standing, -G.stance_height_m, abs_tol=1e-9)
    assert math.isclose(sitting, -body.sit_height_m, abs_tol=1e-9)


def test_pose_that_the_legs_cannot_hold_raises():
    body, _ = simulated_body()
    body.set_pose(BodyPose(z=0.30))
    with pytest.raises(ReachError):
        body.targets(0.02)


# --- servo bus ------------------------------------------------------------------------------------
def test_packet_checksum_and_layout():
    frame = sv.build(1, sv.WRITE, bytes([sv.REG_GOAL_POSITION, 0x00, 0x08]))
    assert frame[:2] == b"\xff\xff" and frame[2] == 1 and frame[3] == 5 and frame[4] == sv.WRITE
    assert frame[-1] == (~(1 + 5 + sv.WRITE + sv.REG_GOAL_POSITION + 0x00 + 0x08)) & 0xFF
    reply = sv.parse(
        frame[:4]
        + b"\x00"
        + frame[5:-1]
        + bytes([sv.checksum(bytes([1, 5, 0, sv.REG_GOAL_POSITION, 0, 8]))])
    )
    assert reply.servo_id == 1 and reply.error == 0
    with pytest.raises(sv.ServoError):
        sv.parse(frame[:-1] + b"\x00")


def test_sync_write_moves_all_servos_in_one_frame():
    io = LoopbackTransport()
    bus = Bus(io)
    bus.move({1: 0.0, 2: math.pi / 2, 3: -math.pi / 2}, time_ms=0, speed=0)
    (frame,) = io.frames
    assert frame[2] == sv.BROADCAST and frame[4] == sv.SYNC_WRITE
    assert (
        frame[5] == sv.REG_GOAL_POSITION and frame[6] == 6
    )  # position, time, speed: 3 x u16 per servo
    rows = frame[7:-1]
    assert len(rows) == 3 * 7
    ids = [rows[i * 7] for i in range(3)]
    positions = [struct.unpack("<H", rows[i * 7 + 1 : i * 7 + 3])[0] for i in range(3)]
    assert ids == [1, 2, 3] and positions == [2048, 3072, 1024]
    assert frame[-1] == sv.checksum(frame[2:-1])


def test_calibration_sign_and_offset():
    cal = Calibration(sign={5: -1}, offset_ticks={5: 10})
    t = cal.to_ticks(5, math.radians(45))
    assert t == 2048 - 512 + 10
    assert math.isclose(cal.to_radians(5, t), math.radians(45), abs_tol=1e-9)
    assert Calibration().to_ticks(1, math.pi) == 4095  # clamped to the turn


def test_reads_decode_position_temperature_and_load():
    io = LoopbackTransport()
    bus = Bus(io)
    io.queue_reply(4, struct.pack("<H", 2048 + 1024))
    assert math.isclose(bus.position(4), math.pi / 2, abs_tol=1e-9)
    io.queue_reply(4, bytes([41]))
    assert bus.temperature(4) == 41
    io.queue_reply(4, struct.pack("<H", 0x400 | 300))
    assert bus.load(4) == -300
    io.replies.clear()
    assert bus.ping(9) is False


def test_bus_actuator_sends_eighteen_joints():
    io = LoopbackTransport()
    act = BusActuator(Bus(io), G)
    body = Body(geometry=G, actuator=act)
    body.tick(0.02)
    (frame,) = io.frames
    assert (len(frame) - 8) // 7 == 18
    assert act.servo_ids() == list(range(1, 19))


# --- backend -----------------------------------------------------------------------------------
def test_backend_walks_then_settles(monkeypatch):
    body, sim = simulated_body(gait=GaitParams(cycle_s=0.4))
    body.stand(actuate=True)
    monkeypatch.setattr("pincer_robot.body.controller.time.sleep", lambda s: None)
    robot = BodyBackend(body)
    out = robot.walk("forward", 0.05, speed=0.5)
    assert out.startswith("OK")
    assert all(body.feet_down())
    assert sim.report().stable
    assert robot.get_status().is_moving is False
