from pincer_robot.presence.fusion import FusionConfig, G, PresenceFusion, PresenceState

CFG = FusionConfig(
    person_on=0.6,
    person_off=0.3,
    person_on_frames=2,
    person_off_frames=5,
    lidar_stale_s=3.0,
    owner_on=0.7,
    owner_off=0.4,
    owner_on_frames=2,
    owner_grace_s=5.0,
    session_timeout_s=60.0,
    voice_ttl_s=30.0,
    moved_accel_threshold=3.0,
    moved_tilt_deg=25.0,
    moved_hold_s=10.0,
    face_stale_s=2.0,
)


def _fusion() -> PresenceFusion:
    return PresenceFusion(CFG)


def _owner_present(f: PresenceFusion, t: float, frames: int = 3, dt: float = 0.2) -> float:
    for _ in range(frames):
        f.update_lidar(0.9, t)
        f.update_face(0.9, t)
        f.tick(t)
        t += dt
    return t


def test_initial_state_is_empty_and_message_shape() -> None:
    f = _fusion()
    s = f.tick(100.0)
    assert s == PresenceState(False, False, False, False, False, 0.0, 0.0, 100.0)
    msg = s.to_message()
    assert msg == {
        "t": "presence",
        "person": False,
        "owner": False,
        "voice_ok": False,
        "duress": False,
        "moved": False,
        "confidence": {"lidar": 0.0, "face": 0.0},
        "ts": 100,
    }


def test_message_matches_protocol_example_keys() -> None:
    s = PresenceState(True, True, False, False, False, 0.923, 0.884, 1791600000.6)
    msg = s.to_message()
    assert list(msg.keys()) == [
        "t", "person", "owner", "voice_ok", "duress", "moved", "confidence", "ts"
    ]  # fmt: skip
    assert msg["confidence"] == {"lidar": 0.92, "face": 0.88}
    assert msg["ts"] == 1791600000


def test_person_needs_consecutive_frames() -> None:
    f = _fusion()
    f.update_lidar(0.9, 0.0)
    assert not f.tick(0.0).person
    f.update_lidar(0.9, 0.1)
    assert f.tick(0.1).person


def test_person_single_bad_frame_does_not_flip() -> None:
    f = _fusion()
    f.update_lidar(0.9, 0.0)
    f.update_lidar(0.9, 0.1)
    assert f.tick(0.1).person
    f.update_lidar(0.0, 0.2)
    assert f.tick(0.2).person
    # Hysteresis band (between off and on) keeps the state too.
    for i in range(10):
        f.update_lidar(0.45, 0.3 + i * 0.1)
        assert f.tick(0.3 + i * 0.1).person


def test_person_turns_off_after_off_frames() -> None:
    f = _fusion()
    f.update_lidar(0.9, 0.0)
    f.update_lidar(0.9, 0.1)
    f.tick(0.1)
    t = 0.2
    for i in range(5):
        f.update_lidar(0.1, t)
        s = f.tick(t)
        assert s.person == (i < 4)
        t += 0.1


def test_person_off_when_lidar_stale() -> None:
    f = _fusion()
    f.update_lidar(0.9, 0.0)
    f.update_lidar(0.9, 0.1)
    assert f.tick(0.1).person
    assert f.tick(2.0).person
    s = f.tick(3.5)
    assert not s.person
    assert s.lidar_conf == 0.0


def test_live_face_counts_as_person_without_lidar() -> None:
    f = _fusion()
    f.update_face(0.2, 0.0, live_present=True)
    assert f.tick(0.0).person
    assert not f.tick(2.5).person


def test_owner_hysteresis_on_and_grace() -> None:
    f = _fusion()
    f.update_face(0.9, 0.0)
    assert not f.tick(0.0).owner  # one strong frame is not enough
    f.update_face(0.9, 0.2)
    assert f.tick(0.2).owner
    # A bad frame, then a weak-but-ok frame: still owner.
    f.update_face(0.0, 0.4)
    assert f.tick(0.4).owner
    f.update_face(0.5, 0.6)
    assert f.tick(0.6).owner
    # Below owner_off for longer than the grace: owner lost.
    f.update_face(0.1, 3.0)
    assert f.tick(3.0).owner
    f.update_face(0.1, 5.5)
    assert f.tick(5.5).owner
    f.update_face(0.1, 5.7)
    assert not f.tick(5.7).owner
    # Needs two strong frames again.
    f.update_face(0.9, 6.0)
    assert not f.tick(6.0).owner
    f.update_face(0.9, 6.2)
    assert f.tick(6.2).owner


def test_owner_face_gone_entirely_drops_after_grace() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)  # last face frame was at t - 0.2
    assert f.tick(t).owner
    assert f.tick(t + 4.7).owner  # 4.9 s since the last frame: inside the grace
    assert not f.tick(t + 4.9).owner  # 5.1 s: dropped


def test_session_timeout_without_strong_match() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    # Keep feeding weak-but-above-off frames: owner survives the grace but not the session.
    while t < 70.0:
        f.update_face(0.5, t)
        s = f.tick(t)
        assert s.owner == (t <= 60.4)
        t += 1.0


def test_voice_ok_ttl_and_requires_owner() -> None:
    f = _fusion()
    f.update_voice(True, 0.0)
    assert not f.tick(0.0).voice_ok  # no owner -> no voice_ok
    t = _owner_present(f, 0.0)
    f.update_voice(True, t)
    assert f.tick(t).voice_ok
    f.update_face(0.9, t + 29.0)
    assert f.tick(t + 29.0).voice_ok
    f.update_face(0.9, t + 31.0)
    assert not f.tick(t + 31.0).voice_ok
    f.update_voice(True, t + 31.0)
    f.update_face(0.9, t + 31.0)
    assert f.tick(t + 31.0).voice_ok
    f.update_voice(False, t + 32.0)
    assert not f.tick(t + 32.0).voice_ok


def test_voice_ok_cleared_when_owner_lost() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    f.update_voice(True, t)
    assert f.tick(t).voice_ok
    assert not f.tick(t + 6.0).owner
    _owner_present(f, t + 6.0)
    assert not f.tick(t + 7.0).voice_ok


def test_moved_from_accel_spike_holds_and_clears_voice() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    f.update_voice(True, t)
    assert f.tick(t).voice_ok
    f.update_imu((0.0, 0.0, G), t)  # at rest
    assert not f.tick(t).moved
    f.update_imu((2.0, 0.0, G + 4.0), t + 0.1)  # lifted / knocked
    s = f.tick(t + 0.1)
    assert s.moved
    assert not s.voice_ok
    f.update_face(0.9, t + 9.0)
    assert f.tick(t + 9.0).moved
    f.update_face(0.9, t + 10.2)
    assert not f.tick(t + 10.2).moved
    assert not f.tick(t + 10.2).voice_ok  # voice stays cleared until re-verified


def test_moved_from_tilt_and_explicit_pose_hook() -> None:
    f = _fusion()
    f.update_imu((0.0, 0.0, G), 0.0, tilt_deg=30.0)
    assert f.tick(0.0).moved
    g = _fusion()
    g.mark_moved(5.0, "odometry")
    assert g.tick(5.0).moved
    assert not g.tick(15.1).moved


def test_duress_latches_and_does_not_change_other_fields() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    f.update_voice(True, t)
    before = f.tick(t)
    f.raise_duress(t, "word")
    after = f.tick(t)
    assert after.duress
    assert (after.person, after.owner, after.voice_ok, after.moved) == (
        before.person,
        before.owner,
        before.voice_ok,
        before.moved,
    )
    assert f.tick(t + 1000.0).duress
    f.clear_duress()
    assert not f.tick(t + 1000.0).duress
    assert any(ev == "duress:word" for _, ev in f.events)


def test_signature_only_tracks_booleans() -> None:
    a = PresenceState(True, False, False, False, False, 0.5, 0.1, 1.0)
    b = PresenceState(True, False, False, False, False, 0.9, 0.3, 2.0)
    c = PresenceState(True, True, False, False, False, 0.9, 0.9, 3.0)
    assert a.signature() == b.signature()
    assert a.signature() != c.signature()


def test_face_stale_zeroes_confidence_but_grace_holds_owner() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    s = f.tick(t + 3.0)
    assert s.owner
    assert s.face_conf == 0.0


def test_events_log_transitions() -> None:
    f = _fusion()
    t = _owner_present(f, 0.0)
    f.tick(t + 10.0)
    names = [e for _, e in f.events]
    assert "person:on" in names and "owner:on" in names and "owner:off" in names


def test_feature_fields_omitted_by_default_and_present_when_enabled() -> None:
    f = _fusion()
    f.update_lidar(0.9, 0.0, persons=2)
    f.update_face(0.9, 0.0, owners=("can", "spouse"))
    msg = f.tick(0.0).to_message()
    assert "persons" not in msg and "owners" not in msg
    assert "room_match" not in msg and "network" not in msg and "room" not in msg
    f.two_person = True
    f.set_extras(room_match=0.934, network=False, room="bedroom", room_known=True)
    s = f.tick(0.1)
    msg = s.to_message()
    assert msg["persons"] == 2 and msg["owners"] == ["can", "spouse"]
    assert msg["room_match"] == 0.93 and msg["network"] is False and msg["room"] == "bedroom"
    # room is sent as null when a map exists but the pose is in no named room.
    f.set_extras(room=None, room_known=True)
    assert f.tick(0.2).to_message()["room"] is None
    f.set_extras()
    assert "room" not in f.tick(0.3).to_message()


def test_persons_and_owners_go_stale_with_their_sensors() -> None:
    f = _fusion()
    f.two_person = True
    f.update_lidar(0.9, 0.0, persons=2)
    f.update_face(0.9, 0.0, owners=("can",))
    s = f.tick(0.0)
    assert s.persons == 2 and s.owners == ("can",)
    s = f.tick(4.0)  # lidar stale (3 s), face stale (2 s)
    assert s.persons == 0 and s.owners == ()


def test_signature_includes_counts_and_network() -> None:
    a = PresenceState(True, True, False, False, False, 0.9, 0.9, 1.0, persons=1, network=True)
    b = PresenceState(True, True, False, False, False, 0.9, 0.9, 2.0, persons=2, network=True)
    c = PresenceState(True, True, False, False, False, 0.9, 0.9, 3.0, persons=2, network=False)
    d = PresenceState(True, True, False, False, False, 0.9, 0.9, 3.0, persons=2, network=False,
                      room_match=0.1)  # fmt: skip
    assert a.signature() != b.signature() != c.signature()
    assert c.signature() == d.signature()  # room_match rides on the heartbeat


def test_note_owner_seen_keeps_last_room_and_bearing() -> None:
    f = _fusion()
    f.note_owner_seen(1.0, 0.4, "desk")
    f.note_owner_seen(2.0, None, None)
    assert f.last_seen_room == "desk" and f.last_seen_bearing == 0.4 and f.last_seen_ts == 2.0


def test_battery_and_dock_extras_on_the_wire_and_in_signature() -> None:
    f = _fusion()
    assert "battery" not in f.tick(0.0).to_message()
    f.set_extras(battery=(42, 11.52, False), dock="idle")
    msg = f.tick(0.1).to_message()
    assert msg["battery"] == {"pct": 42, "voltage": 11.52, "charging": False}
    assert msg["dock"] == "idle"
    a = f.tick(0.2)
    f.set_extras(battery=(41, 11.50, False), dock="idle")
    b = f.tick(0.3)
    assert a.signature() == b.signature()  # pct / voltage ride on the heartbeat
    f.set_extras(battery=(41, 11.50, True), dock="charging")
    c = f.tick(0.4)
    assert c.signature() != b.signature()  # charging flag and dock state push at once
