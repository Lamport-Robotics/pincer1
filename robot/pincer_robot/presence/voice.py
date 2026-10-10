"""Passphrase check: speech-to-text + speaker verification.

``PassphraseVerifier.check`` answers *ok* only if (a) the transcript contains the
enrolled passphrase (exact-normalised or phonetic match against its hashes) and
(b) the speaker embedding of the same audio is close enough to the enrolled voice.

Backends:
* STT: ``WhisperSTT`` (faster-whisper / CTranslate2). On the Jetson: ``small`` on
  ``device="cuda"`` with ``compute_type="float16"``; on a Pi / laptop CPU: ``base``
  with ``int8``. ``compute.ComputePlan`` picks these.
* Speaker: ``ResemblyzerEncoder`` (GE2E d-vector, 256-d; cosine > ~0.75 = same speaker),
  on torch CUDA when torch sees the GPU, else CPU.
Both are imported lazily; ``MockSTT`` / ``MockSpeakerEncoder`` cover tests and ``--mock``.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Protocol

import numpy as np

from ..hashing import normalize_phrase, phrase_matches, windows


class SpeechToText(Protocol):
    def transcribe(self, audio: np.ndarray, sample_rate: int) -> str: ...


class SpeakerEncoder(Protocol):
    def embed(self, audio: np.ndarray, sample_rate: int) -> np.ndarray: ...


@dataclass(frozen=True)
class VoiceResult:
    ok: bool
    transcript: str
    phrase_ok: bool
    speaker_ok: bool
    speaker_score: float


def _cos(a: np.ndarray, b: np.ndarray) -> float:
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    na, nb = float(np.linalg.norm(a)), float(np.linalg.norm(b))
    if na == 0 or nb == 0:
        return 0.0
    return float(np.dot(a, b) / (na * nb))


def transcript_has_phrase(transcript: str, hashes: list[str], n_words: int) -> bool:
    """Slide an ``n_words`` window over the transcript and test each against the hashes."""
    words = normalize_phrase(transcript).split()
    if not words or not hashes:
        return False
    sizes = {n_words} if n_words > 0 else {1, 2, 3, 4}
    for size in sorted(sizes):
        for cand in windows(words, size):
            if phrase_matches(cand, hashes):
                return True
    # Whisper may glue words ("opensesame") - also test the whole normalised string.
    return phrase_matches("".join(words), hashes) if len(words) > 1 else False


class PassphraseVerifier:
    def __init__(
        self,
        stt: SpeechToText,
        encoder: SpeakerEncoder | None,
        passphrase_hashes: list[str],
        passphrase_words: int,
        enrolled_voice: np.ndarray | None,
        speaker_threshold: float = 0.75,
    ) -> None:
        self.stt = stt
        self.encoder = encoder
        self.hashes = list(passphrase_hashes)
        self.n_words = passphrase_words
        self.enrolled_voice = (
            np.asarray(enrolled_voice, dtype=np.float64) if enrolled_voice is not None else None
        )
        self.speaker_threshold = speaker_threshold

    def check(
        self, audio: np.ndarray, sample_rate: int, transcript: str | None = None
    ) -> VoiceResult:
        text = transcript if transcript is not None else self.stt.transcribe(audio, sample_rate)
        phrase_ok = transcript_has_phrase(text, self.hashes, self.n_words)
        if self.encoder is None or self.enrolled_voice is None:
            # No speaker model enrolled: voice is passphrase-only (documented weaker mode).
            return VoiceResult(phrase_ok, text, phrase_ok, True, 1.0)
        if not phrase_ok:
            return VoiceResult(False, text, False, False, 0.0)
        emb = self.encoder.embed(audio, sample_rate)
        score = _cos(emb, self.enrolled_voice)
        speaker_ok = score >= self.speaker_threshold
        return VoiceResult(speaker_ok, text, True, speaker_ok, score)


# --- mocks ----------------------------------------------------------------------------


class MockSTT:
    def __init__(self, text: str = "") -> None:
        self.text = text
        self.calls = 0

    def transcribe(self, audio: np.ndarray, sample_rate: int) -> str:
        self.calls += 1
        return self.text


class MockSpeakerEncoder:
    def __init__(self, embedding: np.ndarray | None = None) -> None:
        self.embedding = (
            np.asarray(embedding, dtype=np.float64) if embedding is not None else np.ones(256)
        )

    def embed(self, audio: np.ndarray, sample_rate: int) -> np.ndarray:
        return self.embedding


# --- real backends (lazy) ----------------------------------------------------------------


class WhisperSTT:
    def __init__(
        self,
        model: str = "base",
        language: str | None = None,
        compute_type: str = "int8",
        device: str = "cpu",
    ) -> None:
        from faster_whisper import WhisperModel  # type: ignore[import-not-found]

        self.model_name = model
        self.device = device
        self.compute_type = compute_type
        self._model = WhisperModel(model, device=device, compute_type=compute_type)
        self._language = language

    @classmethod
    def from_plan(cls, plan: Any, language: str | None = None) -> WhisperSTT:
        return cls(plan.whisper_model, language, plan.whisper_compute_type, plan.whisper_device)

    def transcribe(self, audio: np.ndarray, sample_rate: int) -> str:
        pcm = _to_float_mono(audio)
        if sample_rate != 16000:
            pcm = _resample(pcm, sample_rate, 16000)
        segments, _info = self._model.transcribe(
            pcm, language=self._language, beam_size=1, vad_filter=True
        )
        return " ".join(s.text.strip() for s in segments).strip()


class ResemblyzerEncoder:
    """GE2E speaker encoder. ``device`` is a torch device string (``"cuda"``/``"cpu"``);
    ``ComputePlan.torch_device`` is ``cuda`` only when torch itself sees the GPU."""

    def __init__(self, device: str = "cpu") -> None:
        from resemblyzer import VoiceEncoder  # type: ignore[import-not-found]

        self.device = device
        self._enc = VoiceEncoder(device)

    def embed(self, audio: np.ndarray, sample_rate: int) -> np.ndarray:
        from resemblyzer import preprocess_wav  # type: ignore[import-not-found]

        pcm = _to_float_mono(audio)
        wav = preprocess_wav(pcm, source_sr=sample_rate)
        return np.asarray(self._enc.embed_utterance(wav), dtype=np.float64)


def _to_float_mono(audio: np.ndarray) -> np.ndarray:
    a = np.asarray(audio)
    if a.ndim == 2:
        a = a.mean(axis=1)
    if a.dtype.kind in "iu":
        a = a.astype(np.float32) / float(np.iinfo(a.dtype).max)
    return a.astype(np.float32)


def _resample(pcm: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    n_out = int(round(len(pcm) * sr_out / sr_in))
    x_old = np.linspace(0.0, 1.0, num=len(pcm), endpoint=False)
    x_new = np.linspace(0.0, 1.0, num=n_out, endpoint=False)
    return np.interp(x_new, x_old, pcm).astype(np.float32)


def rms(audio: np.ndarray) -> float:
    a = _to_float_mono(audio)
    return float(np.sqrt(np.mean(a * a))) if a.size else 0.0
