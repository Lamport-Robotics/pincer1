"""Duress word detection in speech-to-text output.

The duress word is never stored or logged in plaintext. ``DuressDetector.scan``
slides a window of the enrolled word count over the transcript and tests each
window against the stored hashes (exact and phonetic, see ``hashing``), so STT
spelling slips still trigger it. On a hit it calls the ``on_duress`` hook once and
latches until ``reset``. The service never acknowledges duress out loud and the
presence object does not change shape; only ``duress: true`` goes to the daemon,
which switches the controller to the decoy key.
"""

from __future__ import annotations

import logging
from collections.abc import Callable

from .hashing import normalize_phrase, phrase_matches, windows

log = logging.getLogger("pincer.duress")


class DuressDetector:
    def __init__(
        self,
        hashes: list[str] | None,
        n_words: int = 1,
        on_duress: Callable[[str], None] | None = None,
    ) -> None:
        self.hashes = list(hashes or [])
        self.n_words = max(1, n_words)
        self._on_duress = on_duress
        self.active = False

    @property
    def enabled(self) -> bool:
        return bool(self.hashes)

    def matches(self, transcript: str) -> bool:
        """Pure check: does the transcript contain the duress word? No side effects."""
        if not self.hashes:
            return False
        words = normalize_phrase(transcript).split()
        if not words:
            return False
        sizes = {self.n_words, max(1, self.n_words - 1), self.n_words + 1}
        for size in sorted(sizes):
            for cand in windows(words, size):
                if phrase_matches(cand, self.hashes):
                    return True
        if len(words) > 1 and phrase_matches("".join(words), self.hashes):
            return True
        return False

    def scan(self, transcript: str) -> bool:
        """Check a transcript; raise duress (once) if the word is present."""
        if self.matches(transcript):
            self.raise_duress("word")
            return True
        return False

    def raise_duress(self, source: str = "word") -> None:
        """Explicit hook: wire any other duress expression (gesture, hidden button) here."""
        if self.active:
            return
        self.active = True
        # Deliberately no transcript or word in the log line.
        log.warning("duress raised (source=%s)", source)
        if self._on_duress is not None:
            self._on_duress(source)

    def reset(self) -> None:
        self.active = False
