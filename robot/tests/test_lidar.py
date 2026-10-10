import math

import numpy as np

from pincer_robot.presence.lidar import LidarConfig, PersonDetector, cluster_points
from pincer_robot.presence.synthetic import Cylinder, chair_at, make_scan, person_at

LEARN = 5.0


def _detector() -> PersonDetector:
    return PersonDetector(LidarConfig(learn_seconds=LEARN))


def _learn(det: PersonDetector, objects: list[Cylinder] | None = None, hz: float = 10.0) -> float:
    """Feed scans until the background is learned; returns the next timestamp."""
    rng = np.random.default_rng(0)
    t = 0.0
    n = int(LEARN * hz) + 2
    for _ in range(n):
        det.update(make_scan(t, objects, noise_m=0.01, rng=rng))
        t += 1.0 / hz
    assert not det.learning
    return t


def test_empty_room_walls_only_no_person() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(1)
    for _ in range(30):
        res = det.update(make_scan(t, noise_m=0.01, rng=rng))
        t += 0.1
        assert res.confidence == 0.0
        assert res.bearing is None


def test_static_chair_placed_after_learning_is_not_a_person() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(2)
    confs = []
    for _ in range(50):
        res = det.update(make_scan(t, [chair_at(1.2, -0.6)], noise_m=0.01, rng=rng))
        t += 0.1
        confs.append(res.confidence)
    assert max(confs) < 0.5  # person-sized but never moved: size-only confidence


def test_walking_person_detected_with_bearing() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(3)
    confs = []
    bearings = []
    for i in range(40):
        y = -0.8 + i * 0.04  # ~0.4 m/s across the room at 1.5 m
        res = det.update(make_scan(t, [person_at(1.5, y)], noise_m=0.01, rng=rng))
        t += 0.1
        confs.append(res.confidence)
        if res.bearing is not None:
            bearings.append((res.bearing, math.atan2(y, 1.5)))
    assert max(confs) >= 0.9
    assert confs[-1] >= 0.9
    # Bearing tracks the true direction within ~5 degrees once seen.
    err = [abs(b - true) for b, true in bearings[-10:]]
    assert max(err) < math.radians(5)


def test_person_walking_during_learning_is_not_baked_into_background() -> None:
    det = _detector()
    rng = np.random.default_rng(4)
    t = 0.0
    for i in range(int(LEARN * 10) + 2):
        y = -1.5 + (i % 30) * 0.1
        det.update(make_scan(t, [person_at(1.5, y)], noise_m=0.01, rng=rng))
        t += 0.1
    assert not det.learning
    # The same person keeps walking: still detected after learning.
    confs = []
    for i in range(40):
        y = -0.8 + i * 0.04
        res = det.update(make_scan(t, [person_at(1.5, y)], noise_m=0.01, rng=rng))
        t += 0.1
        confs.append(res.confidence)
    assert max(confs) >= 0.9


def test_wide_moving_object_is_not_a_person() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(5)
    confs = []
    for i in range(40):
        y = -0.8 + i * 0.04
        res = det.update(make_scan(t, [Cylinder(2.0, y, 0.6)], noise_m=0.01, rng=rng))
        t += 0.1
        confs.append(res.confidence)
    assert max(confs) == 0.0  # 1.2 m wide: furniture being carried, not a person


def test_too_close_and_too_far_filtered() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(6)
    for _ in range(20):
        res = det.update(make_scan(t, [person_at(0.15, 0.0)], noise_m=0.0, rng=rng))
        t += 0.1
    assert res.confidence == 0.0
    det2 = PersonDetector(LidarConfig(learn_seconds=LEARN))
    t = _learn(det2)
    from pincer_robot.presence.synthetic import Room

    big = Room(-1.0, 8.0, -2.0, 2.0)
    for _ in range(int(LEARN * 10) + 2):
        det2.update(make_scan(t, room=big))
        t += 0.1
    for i in range(20):
        res = det2.update(make_scan(t, [person_at(6.0, -0.4 + i * 0.04)], room=big))
        t += 0.1
    assert res.confidence == 0.0


def test_cluster_points_wraps_around_360() -> None:
    angles = np.linspace(-math.pi, math.pi, 360, endpoint=False)
    xy = np.stack([np.cos(angles), np.sin(angles)], axis=1)  # unit circle
    mask = np.zeros(360, dtype=bool)
    mask[:3] = True
    mask[-3:] = True  # a blob straddling the +-180 degree seam
    clusters = cluster_points(xy, mask, gap=0.1, min_points=3)
    assert len(clusters) == 1
    assert len(clusters[0].points) == 6


def test_learning_phase_reports_zero() -> None:
    det = _detector()
    res = det.update(make_scan(0.0, [person_at(1.0, 0.0)]))
    assert res.learning
    assert res.confidence == 0.0


def _walk(det: PersonDetector, t: float, bodies: int, frames: int = 40) -> list[int]:
    rng = np.random.default_rng(9)
    counts = []
    for i in range(frames):
        y = -0.8 + i * 0.04
        objs = [person_at(1.5, y)]
        if bodies >= 2:
            objs.append(person_at(2.2, -y - 0.6))  # second body walking the other way
        res = det.update(make_scan(t, objs, noise_m=0.01, rng=rng))
        t += 0.1
        counts.append(res.persons)
    return counts


def test_two_bodies_count_as_two_persons() -> None:
    det = _detector()
    t = _learn(det)
    counts = _walk(det, t, bodies=2)
    assert counts[-1] == 2
    assert max(counts) == 2


def test_one_body_counts_as_one_person() -> None:
    det = _detector()
    t = _learn(det)
    counts = _walk(det, t, bodies=1)
    assert counts[-1] == 1 and max(counts) == 1


def test_static_chair_is_not_counted_as_a_person() -> None:
    det = _detector()
    t = _learn(det)
    rng = np.random.default_rng(10)
    for _ in range(30):
        res = det.update(make_scan(t, [chair_at(1.2, -0.6)], noise_m=0.01, rng=rng))
        t += 0.1
    assert res.persons == 0
    assert det.background is not None and det.static_scan() is not None
