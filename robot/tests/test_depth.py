import math

from pincer_robot.depth import FloorWatch, MockDepthSource, mock_scene
from pincer_robot.terrain import CameraPose

HEAD = CameraPose(x=0.07, z=0.40, tilt=math.radians(-25))


def watch(scene: str, pose=HEAD) -> FloorWatch:
    return FloorWatch(
        MockDepthSource(lambda: pose, scene=mock_scene(scene)), lambda: pose, stride=1
    )


def test_flat_floor_is_clear_and_far():
    r = watch("flat").update(now=1.0)
    assert r is not None and r.clearance_m is None and r.step is None
    assert r.walkable_m == math.inf and r.seen_cells > 50 and r.ts == 1.0


def test_step_scene_reports_the_riser():
    r = watch("step").update()
    assert r is not None and r.step is not None
    assert abs(r.step.distance_m - 0.6) < 0.05 and abs(r.step.riser_m - 0.12) < 0.02
    assert abs(r.walkable_m - 0.6) < 0.05


def test_stairs_scene_reads_the_first_of_three():
    r = watch("stairs").update()
    assert r is not None and r.step is not None
    assert abs(r.step.riser_m - 0.17) < 0.02 and abs(r.step.distance_m - 0.7) < 0.05


def test_wall_and_drop_limit_the_walk():
    wall = watch("wall").update()
    assert wall is not None and wall.clearance_m is not None and abs(wall.clearance_m - 0.5) < 0.06
    drop = watch("drop").update()
    assert drop is not None and drop.clearance_m is not None and abs(drop.clearance_m - 0.6) < 0.08
    assert drop.walkable_m < 0.7


def test_head_tilt_is_read_every_frame():
    tilt = {"v": math.radians(-25)}
    pose = lambda: CameraPose(x=0.07, z=0.40, tilt=tilt["v"])  # noqa: E731
    w = FloorWatch(MockDepthSource(pose, scene=mock_scene("step")), pose, stride=1)
    a = w.update()
    tilt["v"] = math.radians(-35)  # the gaze looked further down: same scene, same answer
    b = w.update()
    assert a is not None and b is not None and a.step is not None and b.step is not None
    assert abs(a.step.distance_m - b.step.distance_m) < 0.06
