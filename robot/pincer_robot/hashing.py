"""Hashing of spoken secrets (passphrase, duress word) and their fuzzy keys.

Only hashes are stored on disk. Because the words arrive through speech-to-text
we cannot do classic fuzzy string matching against a hash, so each secret is
stored twice:

* ``exact``   - scrypt of the normalised phrase (lowercase, letters only, single spaces)
* ``phonetic``- scrypt of a crude sound key (``phonetic_key``) that collapses the
  typical STT spelling variants ("pine apple" / "pineapple" / "pinapple").

Verification tries both. This keeps the plaintext off disk while tolerating the
kind of errors Whisper actually makes. A spoken one-or-two-word secret has low
entropy no matter what, so the KDF cost here is deliberately moderate: it is a
privacy measure for the enrolment file, not what protects the key (the SE050 and
the touch button are).
"""

from __future__ import annotations

import hashlib
import hmac
import os
import re
from functools import lru_cache

_SCRYPT_N = 2**13
_SCRYPT_R = 8
_SCRYPT_P = 1
_DKLEN = 32
_PREFIX = "scrypt"

_NON_ALPHA = re.compile(r"[^a-z\s]+")
_WS = re.compile(r"\s+")


def normalize_phrase(text: str) -> str:
    """Lowercase, ascii-letters-only, single-spaced. Used for the exact hash."""
    text = text.lower()
    text = text.replace("'", "")
    text = _NON_ALPHA.sub(" ", text)
    return _WS.sub(" ", text).strip()


def phonetic_key(text: str) -> str:
    """A deliberately crude, language-agnostic sound key.

    Not Metaphone; just enough to absorb Whisper's common slips: spacing,
    doubled consonants, c/k/q, ph/f, z/s, vowel substitutions after the first
    letter of the phrase.
    """
    s = normalize_phrase(text).replace(" ", "")
    if not s:
        return ""
    s = s.replace("ph", "f").replace("ck", "k").replace("qu", "k")
    s = s.replace("c", "k").replace("q", "k").replace("z", "s").replace("x", "ks")
    s = s.replace("w", "v").replace("y", "i").replace("gh", "")
    first, rest = s[0], s[1:]
    rest = re.sub(r"[aeiou]", "", rest)
    out = first
    for ch in rest:
        if ch != out[-1]:
            out += ch
    return out


def hash_secret(text: str, *, salt: bytes | None = None) -> str:
    """scrypt hash of ``text`` (already normalised by the caller) as a self-describing string."""
    salt = salt if salt is not None else os.urandom(16)
    dk = hashlib.scrypt(
        text.encode("utf-8"),
        salt=salt,
        n=_SCRYPT_N,
        r=_SCRYPT_R,
        p=_SCRYPT_P,
        dklen=_DKLEN,
    )
    return f"{_PREFIX}${_SCRYPT_N}${_SCRYPT_R}${_SCRYPT_P}${salt.hex()}${dk.hex()}"


def _parse(stored: str) -> tuple[int, int, int, bytes, bytes] | None:
    parts = stored.split("$")
    if len(parts) != 6 or parts[0] != _PREFIX:
        return None
    try:
        return (
            int(parts[1]),
            int(parts[2]),
            int(parts[3]),
            bytes.fromhex(parts[4]),
            bytes.fromhex(parts[5]),
        )
    except ValueError:
        return None


@lru_cache(maxsize=4096)
def _derive(text: str, n: int, r: int, p: int, salt: bytes, dklen: int) -> bytes:
    return hashlib.scrypt(text.encode("utf-8"), salt=salt, n=n, r=r, p=p, dklen=dklen)


def verify_secret(text: str, stored: str) -> bool:
    """Constant-time comparison of ``text`` against a ``hash_secret`` string."""
    parsed = _parse(stored)
    if parsed is None:
        return False
    n, r, p, salt, dk = parsed
    candidate = _derive(text, n, r, p, salt, len(dk))
    return hmac.compare_digest(candidate, dk)


def hash_phrase_pair(phrase: str) -> list[str]:
    """The two hashes we store for a spoken secret: exact + phonetic."""
    return [hash_secret(normalize_phrase(phrase)), hash_secret(phonetic_key(phrase))]


def phrase_matches(candidate: str, hashes: list[str]) -> bool:
    """True if ``candidate`` matches any stored hash, exactly or phonetically."""
    exact = normalize_phrase(candidate)
    key = phonetic_key(candidate)
    if not exact:
        return False
    for h in hashes:
        if verify_secret(exact, h) or (key and verify_secret(key, h)):
            return True
    return False


def windows(words: list[str], size: int) -> list[str]:
    """All contiguous ``size``-word windows of ``words`` joined by spaces."""
    if size <= 0 or len(words) < size:
        return []
    return [" ".join(words[i : i + size]) for i in range(len(words) - size + 1)]
