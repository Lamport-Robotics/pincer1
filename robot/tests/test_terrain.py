import math

import numpy as np
import pytest

from pincer_robot.terrain import (
    DROP,
    FLOOR,
    STEP,
    WALL,
    CameraModel,
    CameraPose,
    classify,
    find_step,
    front_clearance,
    height_grid,
    render_steps,
    unproject,
)

CAM = CameraModel.from_fov(320, 200, 73.0)
HEAD = CameraPose(x=0.07, z=0.36, tilt=math.radians(-25))  # looking down at the floor ahead


def grid_for(**scene):
    depth = render_steps(CAM, HEAD, **scene)
    return height_grid(depth, CAM, HEAD, stride=1)


def test_a_flat_floor_unprojects_to_z_zero():
    pts = unproject(render_steps(CAM, HEAD), CAM, HEAD, stride=1)
    assert len(pts) > 1000
    assert abs(float(np.median(pts[:, 2]))) < 1e-6 and float(np.abs(pts[:, 2]).max()) < 1e-6
    assert pts[:, 0].min() > 0.0


def test_floor_only_has_no_step_and_is_clear():
    g = grid_for()
    labels = classify(g)
    seen = labels != 4
    assert seen.any() and (labels[seen] == FLOOR).all()
    assert find_step(g) is None
    assert front_clearance(g) is None


@pytest.mark.parametrize(
    "riser,edge,yaw",
    [(0.08, 0.45, 0.0), (0.12, 0.55, math.radians(15)), (0.17, 0.40, math.radians(-15))],
)
def test_step_distance_height_and_angle(riser, edge, yaw):
    g = grid_for(riser_m=riser, edge_x_m=edge, edge_yaw=yaw)
    s = find_step(g)
    assert s is not None and s.rows >= 5
    assert abs(s.distance_m - edge) < 0.04
    assert abs(s.riser_m - riser) < 0.015
    assert abs(s.yaw - yaw) < math.radians(5)


def test_tread_depth_is_read_when_the_next_riser_is_in_view():
    g = grid_for(riser_m=0.10, edge_x_m=0.40, tread_m=0.25)
    s = find_step(g)
    assert s is not None and s.tread_m is not None
    assert abs(s.tread_m - 0.25) < 0.06


def test_wall_and_drop_set_the_clearance():
    wall = grid_for(wall_x_m=0.60)
    assert (classify(wall) == WALL).any()
    c = front_clearance(wall)
    assert c is not None and abs(c - 0.60) < 0.06
    assert find_step(wall) is None
    drop = grid_for(drop_x_m=0.70)
    labels = classify(drop)
    assert (labels == DROP).any()
    c = front_clearance(drop)
    assert c is not None and abs(c - 0.70) < 0.08


def test_noise_does_not_break_the_edge():
    depth = render_steps(CAM, HEAD, riser_m=0.12, edge_x_m=0.5, noise_m=0.01)
    s = find_step(height_grid(depth, CAM, HEAD, stride=1))
    assert s is not None
    assert abs(s.distance_m - 0.5) < 0.06 and abs(s.riser_m - 0.12) < 0.03


def test_head_yaw_moves_the_scene_the_other_way():
    # looking 20 degrees left, a square-on step still reads as square-on in the body frame
    pose = CameraPose(x=0.07, z=0.36, tilt=math.radians(-25), yaw=math.radians(20))
    depth = render_steps(CAM, pose, riser_m=0.10, edge_x_m=0.5)
    s = find_step(height_grid(depth, CAM, pose, stride=1))
    assert s is not None and abs(s.yaw) < math.radians(4) and abs(s.distance_m - 0.5) < 0.05
    assert (classify(height_grid(depth, CAM, pose, stride=1)) == STEP).any()
