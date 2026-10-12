import math

import numpy as np

from pincer_robot.presence.lidar import LidarConfig, PersonDetector
from pincer_robot.presence.room import (
    RoomFingerprint,
    RoomMatcher,
    fingerprint_from_static,
    load_fingerprint,
    room_match,
    save_fingerprint,
)
from pincer_robot.presence.synthetic import Cylinder, Room, chair_at, make_scan, person_at

LEARN = 5.0
FURNITURE = [chair_at(1.8, 1.2), chair_at(-1.0, -1.5), chair_at(2.0, -1.3)]


def _static(
    objects: list[Cylinder] | None = None,
    room: Room | None = None,
    pose: tuple[float, float, float] = (0.0, 0.0, 0.0),
    seed: int = 0,
    walker: bool = False,
) -> tuple[np.ndarray, np.ndarray, float]:
    """Learn the background of a synthetic room and return the static scan."""
    det = PersonDetector(LidarConfig(learn_seconds=LEARN))
    rng = np.random.default_rng(seed)
    t = 0.0
    for i in range(int(LEARN * 10) + 3):
        objs = list(objects or [])
        if walker:  # someone walking through during learning must not end up in the map
            objs.append(person_at(1.2, -1.5 + (i % 30) * 0.1))
        det.update(make_scan(t, objs, room=room, pose=pose, noise_m=0.01, rng=rng))
        t += 0.1
    static = det.static_scan()
    assert static is not None
    return static


REF = _static(FURNITURE)
FP = fingerprint_from_static(*REF)


def test_fingerprint_shape() -> None:
    assert len(FP.profile) == 72
    assert all(math.isfinite(v) for v in FP.profile)  # closed room: every sector has a wall
    assert 3 <= len(FP.segments) <= 8
    assert FP.segments == sorted(FP.segments, reverse=True)
    assert FP.segments[0] > 2.5  # the long walls of the 4 x 4 m room


def test_same_room_same_place_matches_fully() -> None:
    assert room_match(FP, *_static(FURNITURE, seed=5)) >= 0.97


def test_same_room_rotated_and_moved_30cm() -> None:
    for pose in [
        (0.0, 0.0, math.pi / 2),
        (0.0, 0.0, math.radians(37)),
        (0.2, 0.22, 0.0),
        (-0.2, 0.22, math.radians(120)),
        (0.3, 0.0, math.radians(-75)),
    ]:
        score = room_match(FP, *_static(FURNITURE, pose=pose, seed=3))
        assert score >= 0.85, (pose, score)


def test_same_room_moved_half_metre_still_high() -> None:
    score = room_match(FP, *_static(FURNITURE, pose=(0.35, 0.35, math.radians(60)), seed=3))
    assert score >= 0.85


def test_different_room_scores_low() -> None:
    other = [
        (Room(-2.0, 3.0, -1.5, 1.5), []),
        (Room(-1.0, 2.0, -1.4, 1.4), [chair_at(1.0, 1.0)]),
        (Room(-2.5, 2.5, -2.5, 2.5), []),
    ]
    for room, objs in other:
        score = room_match(FP, *_static(objs, room=room, seed=4))
        assert score <= 0.5, (room, score)


def test_furniture_moved_slightly_still_matches() -> None:
    moved = [chair_at(1.9, 1.0), chair_at(-1.1, -1.3), chair_at(2.0, -1.3)]
    assert room_match(FP, *_static(moved, seed=6)) >= 0.75
    assert room_match(FP, *_static(moved, pose=(0.15, -0.2, math.radians(200)), seed=6)) >= 0.75


def test_person_walking_during_learning_does_not_change_fingerprint() -> None:
    score = room_match(FP, *_static(FURNITURE, seed=7, walker=True))
    assert score >= 0.9


def test_roundtrip_and_matcher(tmp_path) -> None:  # noqa: ANN001
    path = tmp_path / "room.json"
    save_fingerprint(FP, path)
    assert oct(path.stat().st_mode & 0o777) == "0o600"
    back = load_fingerprint(path)
    assert isinstance(back, RoomFingerprint)
    assert np.allclose(back.profile, FP.profile, atol=1e-3)
    assert back.segments == [round(v, 4) for v in FP.segments]
    assert load_fingerprint(tmp_path / "missing.json") is None

    m = RoomMatcher(tmp_path / "other.json")
    assert not m.enrolled and m.match(*REF) is None
    m.enroll(*REF)
    assert m.enrolled and (tmp_path / "other.json").exists()
    assert m.match(*_static(FURNITURE, pose=(0.1, 0.1, 1.0), seed=8)) >= 0.85
    assert m.last_match is not None
    reloaded = RoomMatcher(tmp_path / "other.json")
    assert reloaded.enrolled
