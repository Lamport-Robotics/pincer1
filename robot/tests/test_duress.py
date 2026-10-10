from pincer_robot.duress import DuressDetector
from pincer_robot.hashing import hash_phrase_pair


def _det(word: str = "pineapple", n_words: int = 1) -> tuple[DuressDetector, list[str]]:
    calls: list[str] = []
    det = DuressDetector(hash_phrase_pair(word), n_words, on_duress=calls.append)
    return det, calls


def test_exact_word_in_sentence_triggers_once() -> None:
    det, calls = _det()
    assert det.scan("I would really like a pineapple right now")
    assert det.active
    assert calls == ["word"]
    assert det.scan("pineapple again")
    assert calls == ["word"]  # latched: hook fires once
    det.reset()
    assert det.scan("pineapple")
    assert calls == ["word", "word"]


def test_fuzzy_stt_variants_trigger() -> None:
    det, _ = _det()
    assert det.matches("pass me the pine apple please")
    assert det.matches("Pinapple!")
    assert det.matches("PINEAPPLE")


def test_unrelated_speech_does_not_trigger() -> None:
    det, calls = _det()
    assert not det.scan("send half a sol to my friend")
    assert not det.scan("apple pie")
    assert not det.scan("")
    assert not det.active
    assert calls == []


def test_two_word_duress_phrase() -> None:
    det, _ = _det("red balloon", n_words=2)
    assert det.matches("i think the red balloon is over there")
    assert det.matches("redballoon")
    assert not det.matches("red car")


def test_disabled_without_hashes() -> None:
    det = DuressDetector(None)
    assert not det.enabled
    assert not det.scan("pineapple")


def test_explicit_hook_raises_without_speech() -> None:
    det, calls = _det()
    det.raise_duress("gesture")
    assert det.active
    assert calls == ["gesture"]
