import numpy as np

from pincer_robot.presence.face import (
    FaceConfig,
    LivenessGate,
    MockFaceRecognizer,
    OwnerFaceMatcher,
    make_mock_observation,
    pose_from_kps,
)

RNG = np.random.default_rng(0)
OWNER = RNG.normal(size=512)
STRANGER = RNG.normal(size=512)
CFG = FaceConfig(min_frames=4, window_s=3.0, min_pose_std_deg=3.0)


def _matcher() -> OwnerFaceMatcher:
    enrolled = np.stack([OWNER + RNG.normal(scale=0.05, size=512) for _ in range(5)])
    return OwnerFaceMatcher(enrolled, CFG)


def test_static_photo_of_owner_is_rejected() -> None:
    m = _matcher()
    confs = []
    for i in range(10):
        obs = make_mock_observation(OWNER, i * 0.2, yaw=0.0, pitch=0.0)
        r = m.update([obs])
        confs.append(r.owner_conf)
    assert r.match_sim > 0.9
    assert not r.live and r.reason == "static_pose"
    assert max(confs) <= CFG.not_live_factor + 1e-9


def test_owner_with_head_motion_passes() -> None:
    m = _matcher()
    r = None
    for i in range(10):
        obs = make_mock_observation(OWNER, i * 0.2, yaw=10.0 * np.sin(i), pitch=3.0 * np.cos(i))
        r = m.update([obs])
    assert r is not None and r.live and r.owner_conf > 0.9
    assert r.present


def test_stranger_with_head_motion_scores_low() -> None:
    m = _matcher()
    r = None
    for i in range(10):
        obs = make_mock_observation(STRANGER, i * 0.2, yaw=10.0 * np.sin(i))
        r = m.update([obs])
    assert r is not None and r.live  # it is a live person...
    assert r.owner_conf < 0.1  # ...but not the owner


def test_identity_swap_inside_window_breaks_liveness() -> None:
    gate = LivenessGate(CFG)
    for i in range(3):
        gate.push(make_mock_observation(OWNER, i * 0.2, yaw=5.0 * i))
    gate.push(make_mock_observation(STRANGER, 0.8, yaw=20.0))
    live, reason = gate.assess()
    assert not live and reason == "identity_unstable"


def test_window_resets_after_long_gap() -> None:
    gate = LivenessGate(CFG)
    for i in range(4):
        gate.push(make_mock_observation(OWNER, i * 0.2, yaw=5.0 * i))
    assert gate.assess()[0]
    gate.push(make_mock_observation(OWNER, 60.0, yaw=0.0))
    assert gate.assess() == (False, "too_few_frames")


def test_no_face_and_best_of_many() -> None:
    m = _matcher()
    r = m.update([])
    assert not r.present and r.owner_conf == 0.0 and r.x_norm is None
    for i in range(6):
        owner = make_mock_observation(OWNER, i * 0.2, yaw=8.0 * (-1) ** i, x_norm=-0.5)
        other = make_mock_observation(STRANGER, i * 0.2, yaw=0.0, x_norm=0.5)
        r = m.update([other, owner])
    assert r.x_norm is not None and r.x_norm < 0  # picked the owner's position
    assert r.owner_conf > 0.9


def test_mock_recognizer_cycles_script_with_fresh_ts() -> None:
    rec = MockFaceRecognizer([[make_mock_observation(OWNER, 0.0)], []])
    assert len(rec.observe(None, 5.0)) == 1
    assert rec.observe(None, 5.1) == []
    assert rec.observe(None, 5.2)[0].ts == 5.2


def test_pose_from_kps_varies_with_nose_offset() -> None:
    base = np.array([[100, 100], [140, 100], [120, 125], [105, 150], [135, 150]], dtype=float)
    yaw0, pitch0, roll0 = pose_from_kps(base)
    turned = base.copy()
    turned[2, 0] += 12  # nose moves toward the right eye
    yaw1, _, _ = pose_from_kps(turned)
    assert abs(roll0) < 1e-6
    assert yaw1 > yaw0 + 10


SPOUSE = RNG.normal(size=512)


def _multi_matcher() -> OwnerFaceMatcher:
    enrolled = np.stack([OWNER + RNG.normal(scale=0.05, size=512) for _ in range(5)])
    spouse = np.stack([SPOUSE + RNG.normal(scale=0.05, size=512) for _ in range(3)])
    return OwnerFaceMatcher(enrolled, CFG, label="can", people={"spouse": spouse})


def test_two_enrolled_faces_matched_lists_both_labels() -> None:
    m = _multi_matcher()
    r = None
    for i in range(6):
        owner = make_mock_observation(OWNER, i * 0.2, yaw=8.0 * (-1) ** i, x_norm=-0.4)
        spouse = make_mock_observation(SPOUSE, i * 0.2, yaw=2.0, x_norm=0.4)
        r = m.update([spouse, owner])
    assert r is not None
    assert set(r.owners) == {"can", "spouse"} and r.faces == 2
    assert r.owner_conf > 0.9  # owner_conf is still the primary owner's


def test_single_face_lists_one_label_and_stranger_none() -> None:
    m = _multi_matcher()
    r = None
    for i in range(4):
        r = m.update([make_mock_observation(SPOUSE, i * 0.2, yaw=5.0 * i)])
    assert r is not None and r.owners == ("spouse",)
    assert r.owner_conf < 0.1  # the spouse is not the key owner
    for i in range(4):
        r = m.update([make_mock_observation(STRANGER, 10 + i * 0.2, yaw=5.0 * i)])
    assert r is not None and r.owners == ()
    assert m.update([]).owners == ()


def test_owners_remembered_across_frames_within_window() -> None:
    m = _multi_matcher()
    m.update([make_mock_observation(OWNER, 0.0)])
    r = m.update([make_mock_observation(SPOUSE, 1.0)])
    assert set(r.owners) == {"can", "spouse"}  # both seen within window_s = 3 s
    r = m.update([make_mock_observation(SPOUSE, 4.5)])
    assert r.owners == ("spouse",)  # owner's sighting aged out
