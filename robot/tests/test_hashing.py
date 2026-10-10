from pincer_robot.hashing import (
    hash_phrase_pair,
    hash_secret,
    normalize_phrase,
    phonetic_key,
    phrase_matches,
    verify_secret,
    windows,
)


def test_normalize_strips_punctuation_and_case() -> None:
    assert normalize_phrase("  Open, SESAME!  ") == "open sesame"
    assert normalize_phrase("it's") == "its"


def test_phonetic_key_absorbs_common_stt_slips() -> None:
    assert phonetic_key("pineapple") == phonetic_key("pine apple")
    assert phonetic_key("pineapple") == phonetic_key("pinapple")
    assert phonetic_key("cat") == phonetic_key("kat")
    assert phonetic_key("phone") == phonetic_key("fone")
    assert phonetic_key("pineapple") != phonetic_key("banana")


def test_hash_roundtrip_and_format() -> None:
    h = hash_secret("open sesame")
    assert h.startswith("scrypt$")
    assert verify_secret("open sesame", h)
    assert not verify_secret("open sesamee", h)
    assert not verify_secret("open sesame", "garbage")


def test_phrase_matches_exact_and_phonetic() -> None:
    hashes = hash_phrase_pair("open sesame")
    assert phrase_matches("Open Sesame.", hashes)
    assert phrase_matches("open sesamy", hashes)  # vowel slip after first letter
    assert not phrase_matches("close sesame", hashes)
    assert not phrase_matches("", hashes)


def test_windows() -> None:
    assert windows(["a", "b", "c"], 2) == ["a b", "b c"]
    assert windows(["a"], 2) == []
    assert windows(["a", "b"], 0) == []
