import numpy as np

from pincer_robot.hashing import hash_phrase_pair
from pincer_robot.presence.voice import (
    MockSpeakerEncoder,
    MockSTT,
    PassphraseVerifier,
    rms,
    transcript_has_phrase,
)

RNG = np.random.default_rng(0)
OWNER_VOICE = RNG.normal(size=256)
OTHER_VOICE = RNG.normal(size=256)
HASHES = hash_phrase_pair("open sesame")
AUDIO = np.zeros(16000, dtype=np.float32)


def _verifier(heard: str, voice: np.ndarray) -> PassphraseVerifier:
    return PassphraseVerifier(MockSTT(heard), MockSpeakerEncoder(voice), HASHES, 2, OWNER_VOICE)


def test_owner_says_passphrase_ok() -> None:
    r = _verifier("Open sesame!", OWNER_VOICE).check(AUDIO, 16000)
    assert r.ok and r.phrase_ok and r.speaker_ok and r.speaker_score > 0.99


def test_passphrase_inside_longer_sentence() -> None:
    r = _verifier("um, open sesame please", OWNER_VOICE).check(AUDIO, 16000)
    assert r.ok


def test_wrong_phrase_fails_before_speaker_check() -> None:
    r = _verifier("close the door", OWNER_VOICE).check(AUDIO, 16000)
    assert not r.ok and not r.phrase_ok and r.speaker_score == 0.0


def test_right_phrase_wrong_speaker_fails() -> None:
    r = _verifier("open sesame", OTHER_VOICE).check(AUDIO, 16000)
    assert not r.ok and r.phrase_ok and not r.speaker_ok


def test_precomputed_transcript_skips_stt() -> None:
    stt = MockSTT("ignored")
    v = PassphraseVerifier(stt, MockSpeakerEncoder(OWNER_VOICE), HASHES, 2, OWNER_VOICE)
    assert v.check(AUDIO, 16000, transcript="open sesame").ok
    assert stt.calls == 0


def test_no_speaker_model_is_passphrase_only() -> None:
    v = PassphraseVerifier(MockSTT("open sesame"), None, HASHES, 2, None)
    r = v.check(AUDIO, 16000)
    assert r.ok and r.speaker_ok and r.speaker_score == 1.0


def test_transcript_has_phrase_handles_glued_words_and_unknown_length() -> None:
    assert transcript_has_phrase("opensesame", HASHES, 2) is False  # single word, n=2 windows
    assert transcript_has_phrase("hello opensesame", HASHES, 0)
    assert not transcript_has_phrase("", HASHES, 2)


def test_rms() -> None:
    assert rms(np.zeros(100)) == 0.0
    assert abs(rms(np.ones(100, dtype=np.float32) * 0.5) - 0.5) < 1e-6
    assert rms(np.array([32767, -32768], dtype=np.int16)) > 0.99
