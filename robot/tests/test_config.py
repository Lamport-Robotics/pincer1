import numpy as np
import pytest

from pincer_robot.config import FeatureFlags, Settings
from pincer_robot.enroll import Enrolment, build_mock_enrolment, load_enrolment, save_enrolment
from pincer_robot.hashing import phrase_matches


def test_env_prefix_and_elevenlabs_aliases(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.setenv("PINCER_WS_PORT", "7499")
    monkeypatch.setenv("PINCER_OWNER_NAME", "Can")
    monkeypatch.setenv("ELEVENLABS_VOICE_ID", "voice123")
    monkeypatch.setenv("ELEVENLABS_API_KEY", "sk-test")
    cfg = Settings(_env_file=None)  # type: ignore[call-arg]
    assert cfg.ws_port == 7499
    assert cfg.owner_name == "Can"
    assert cfg.elevenlabs_voice_id == "voice123"
    assert cfg.elevenlabs_api_key == "sk-test"
    assert cfg.ws_host == "127.0.0.1"


def test_defaults_match_protocol(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("PINCER_WS_PORT", raising=False)
    cfg = Settings(_env_file=None)  # type: ignore[call-arg]
    assert cfg.ws_port == 7402
    assert cfg.heartbeat_s == 2.0
    assert cfg.elevenlabs_api_key is None
    assert cfg.fusion_config().owner_on == cfg.owner_on
    assert cfg.lidar_config().learn_seconds == 30.0
    assert cfg.face_config().min_frames == cfg.liveness_min_frames


def test_mock_enrolment_roundtrip(tmp_path) -> None:  # noqa: ANN001
    cfg = Settings(_env_file=None, mock_passphrase="open sesame", mock_duress_word="pineapple")  # type: ignore[call-arg]
    enrol = build_mock_enrolment(cfg)
    path = tmp_path / "enrol.json"
    save_enrolment(enrol, path)
    assert oct(path.stat().st_mode & 0o777) == "0o600"
    back = load_enrolment(path)
    assert isinstance(back, Enrolment)
    assert back.face_matrix().shape == (3, 512)
    assert back.voice_vector() is not None and back.voice_vector().shape == (256,)
    assert phrase_matches("open sesame", back.passphrase_hashes)
    assert phrase_matches("pine apple", back.duress_hashes)
    text = path.read_text()
    assert "open sesame" not in text and "pineapple" not in text
    assert np.allclose(back.face_matrix(), enrol.face_matrix())


def test_feature_defaults_are_off(monkeypatch) -> None:  # noqa: ANN001
    for var in ("PINCER_ROOM_LOCK", "PINCER_TWO_PERSON", "PINCER_AIRGAP", "PINCER_WAKE",
                "PINCER_DOCK"):  # fmt: skip
        monkeypatch.delenv(var, raising=False)
    cfg = Settings(_env_file=None)  # type: ignore[call-arg]
    assert (cfg.room_lock, cfg.two_person, cfg.airgap, cfg.wake) == (False, False, "off", False)
    assert cfg.dock is False and cfg.dock_at_pct == 20.0 and cfg.undock_at_pct == 90.0
    assert cfg.battery_i2c_addr == 0x40 and cfg.battery_shunt_ohm == 0.002
    assert cfg.airgap_pin == 7 and cfg.airgap_grace_s == 60.0 and cfg.wake_max_secs == 120
    assert cfg.nav_room_priority == ["bedroom", "desk", "living"]
    assert cfg.feature_flags() == FeatureFlags()
    monkeypatch.setenv("PINCER_AIRGAP", "night")
    monkeypatch.setenv("PINCER_WAKE", "1")
    monkeypatch.setenv("PINCER_DOCK", "1")
    cfg = Settings(_env_file=None)  # type: ignore[call-arg]
    assert cfg.feature_flags() == FeatureFlags(airgap="night", wake=True, dock=True)


def test_feature_flags_update_partial_and_validation() -> None:
    flags = FeatureFlags()
    assert flags.update({"t": "settings", "wake": True, "airgap": "strict"}) == ["airgap", "wake"]
    assert flags == FeatureFlags(airgap="strict", wake=True)
    assert flags.update({"wake": True}) == []  # no change
    with pytest.raises(ValueError):
        flags.update({"airgap": "maybe"})
    with pytest.raises(ValueError):
        flags.update({"room_lock": "yes", "wake": False})
    assert flags == FeatureFlags(airgap="strict", wake=True)  # nothing applied on error
    assert flags.to_message() == {
        "t": "settings", "room_lock": False, "two_person": False, "airgap": "strict", "wake": True,
        "dock": False, "mode": "online", "brain": "",
    }  # fmt: skip
    assert flags.update({"dock": True}) == ["dock"]
    with pytest.raises(ValueError):
        flags.update({"dock": "on"})
    assert flags.dock is True
    assert flags.update({"mode": "offline", "brain": "claude"}) == ["mode", "brain"]
    with pytest.raises(ValueError):
        flags.update({"mode": "sometimes"})
    with pytest.raises(ValueError):
        flags.update({"brain": "gemini"})
    assert flags.mode == "offline" and flags.brain == "claude"


def test_enrolment_people_roundtrip(tmp_path) -> None:  # noqa: ANN001
    from pincer_robot.enroll import EnrolledPerson, add_person

    cfg = Settings(_env_file=None, owner_name="can")  # type: ignore[call-arg]
    path = tmp_path / "enrol.json"
    save_enrolment(build_mock_enrolment(cfg), path)
    enrol = add_person(cfg, "spouse", mock=True, path=path)
    assert enrol.labels() == ["can", "spouse"]
    back = load_enrolment(path)
    assert set(back.people_faces()) == {"spouse"}
    assert back.people_faces()["spouse"].shape == (3, 512)
    with pytest.raises(ValueError):
        back.add_person(EnrolledPerson(label="can"))
    add_person(cfg, "spouse", mock=True, path=path)  # re-enrolling replaces, not duplicates
    assert load_enrolment(path).labels() == ["can", "spouse"]


def test_enrolment_file_is_authenticated(tmp_path) -> None:  # noqa: ANN001
    cfg = Settings(_env_file=None, mock_passphrase="open sesame", mock_duress_word="pineapple")  # type: ignore[call-arg]
    enrol = build_mock_enrolment(cfg)
    path = tmp_path / "enrol.json"
    save_enrolment(enrol, path)
    assert (path.stat().st_mode & 0o777) == 0o600
    assert (tmp_path / "enrol.json.key").exists()
    assert ((tmp_path / "enrol.json.key").stat().st_mode & 0o777) == 0o600
    load_enrolment(path)  # intact: fine
    # another account rewrites the file (a new duress word, a new face): refused without the key
    data = path.read_text().replace("}", " }", 1)
    path.write_text(data)
    import pytest

    with pytest.raises(ValueError, match="authentication tag"):
        load_enrolment(path)
    # a file without a tag next to a key is refused too
    save_enrolment(enrol, path)
    (tmp_path / "enrol.json.mac").unlink()
    with pytest.raises(ValueError, match="no authentication tag"):
        load_enrolment(path)
