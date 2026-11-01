import math

import pytest

from pincer_robot.body import BodyGeometry
from pincer_robot.body.climb import (
    ClimbError,
    ClimbParams,
    Keyframe,
    angles_at,
    body_centre_margin,
    climb_cycle,
    duration,
    interpolate,
    seconds_between,
)
from pincer_robot.body.kinematics import ReachError

G = BodyGeometry()


def feet_on(keyframes: list[Keyframe], z: float, legs=range(6)) -> bool:
    last = keyframes[-1]
    return all(abs(last.feet_world[i][2] - z) < 1e-9 for i in legs)


@pytest.mark.parametrize("riser", [0.08, 0.12])
def test_cycle_solves_and_stays_supported(riser):
    params = ClimbParams(riser_m=riser)
    kfs = climb_cycle(G, params)
    assert kfs[0].note == "stand" and kfs[-1].note == "level"
    for kf in kfs:
        assert sum(kf.down) >= 3, kf.note
        assert body_centre_margin(kf.feet_world, kf.down, kf.pose) >= params.margin_min_m, kf.note
        angles_at(G, kf.feet_world, kf.pose)  # inside the joint limits or ReachError
    # the whole cycle ends standing on the landing, every foot at the riser's height
    assert feet_on(kfs, params.terrain.floor_z + riser)
    assert math.isclose(kfs[-1].pose.z, params.terrain.floor_z + riser + G.stance_height_m)
    assert kfs[-1].pose.pitch == 0.0


def test_interpolated_samples_are_reachable_and_supported():
    params = ClimbParams(riser_m=0.10)
    kfs = climb_cycle(G, params)
    samples = list(interpolate(kfs, 0.02))
    pairs = list(zip(kfs, kfs[1:], strict=False))
    assert len(samples) == 1 + sum(max(1, round(seconds_between(a, b) / 0.02)) for a, b in pairs)
    # between two keyframes a foot counts as planted when both ends say so, or when it landed at the
    # previous keyframe and does not move during this one
    i = 1
    for prev, kf in zip(kfs, kfs[1:]):
        n = max(1, round(seconds_between(prev, kf) / 0.02))
        both = [
            (a and b) or (b and pf == kf.feet_world[j])
            for j, (a, b, pf) in enumerate(zip(prev.down, kf.down, prev.feet_world))
        ]
        for _ in range(n):
            feet, pose = samples[i]
            angles_at(G, feet, pose)
            assert body_centre_margin(feet, both, pose) > 0, kf.note
            i += 1
    assert i == len(samples)


def test_time_is_monotone_and_adds_up():
    kfs = climb_cycle(G, ClimbParams(riser_m=0.08))
    assert all(k.seconds > 0 for k in kfs)
    # a big pose change stretches its keyframe, never shortens it
    assert duration(kfs) >= sum(k.seconds for k in kfs)
    # each keyframe becomes a whole number of 20 ms samples; the drift from rounding stays small
    assert abs(len(list(interpolate(kfs, 0.02))) * 0.02 - duration(kfs)) < 0.05 * duration(kfs)


def test_too_tall_a_riser_is_refused_with_the_phase():
    with pytest.raises(ClimbError) as e:
        climb_cycle(G, ClimbParams(riser_m=0.40))
    assert e.value.phase.startswith("leg")


def test_feet_never_land_on_an_edge():
    params = ClimbParams(riser_m=0.10, steps=1)
    edge = params.terrain.edges()[0]
    for kf in climb_cycle(G, params):
        for foot, down in zip(kf.feet_world, kf.down):
            if down:
                assert abs(foot[0] - edge) >= params.edge_margin_m - 1e-9, kf.note


def test_swing_goes_out_and_over_the_riser():
    params = ClimbParams(riser_m=0.10)
    kfs = climb_cycle(G, params)
    ups = [k for k in kfs if k.note.endswith("up")]
    overs = [k for k in kfs if k.note.endswith("over")]
    assert ups and len(ups) == len(overs)
    for up, over in zip(ups, overs):
        leg = int(up.note.split()[1])
        assert up.feet_world[leg][2] == over.feet_world[leg][2]
        assert over.feet_world[leg][2] >= params.terrain.floor_z + params.lift_clearance_m - 1e-9


def test_angles_at_raises_for_an_impossible_pose():
    kfs = climb_cycle(G, ClimbParams(riser_m=0.08))
    feet, pose = kfs[0].feet_world, kfs[0].pose
    with pytest.raises(ReachError):
        angles_at(G, feet, type(pose)(z=0.5))
