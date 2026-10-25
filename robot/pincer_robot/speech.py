"""Text-to-speech and microphone capture.

Speakers (all ``async def say(text)``, serialised by the service so sentences never
overlap):

* ``ElevenLabsSpeaker`` - streams PCM 22.05 kHz from the ElevenLabs API straight into a
  sounddevice output stream. Needs ``ELEVENLABS_API_KEY`` + ``ELEVENLABS_VOICE_ID``.
* ``EspeakSpeaker``     - ``espeak-ng`` subprocess (offline fallback on the Pi).
* ``PrintSpeaker``      - prints; mock mode and tests.

``make_speaker`` picks by ``tts_backend`` ("auto": ElevenLabs if configured, else
espeak-ng if installed, else print). Heavy imports are inside the classes.

``say(text, urgency=0..1)``: the wake-up alarm raises ``urgency`` line by line. ElevenLabs
gets a lower ``stability`` (more expressive, louder-sounding delivery) and a higher
``style``; espeak-ng gets more amplitude (``-a`` 100 -> 200) and a faster rate. Plain
speech is ``urgency=0``. Software gain beyond what the TTS offers belongs to the audio
device (``amixer``), not here.
"""

from __future__ import annotations

import asyncio
import logging
import shutil
import sys
import threading
from typing import Any, Protocol

import numpy as np

from .config import Settings

log = logging.getLogger("pincer.speech")


class Speaker(Protocol):
    async def say(self, text: str, urgency: float = 0.0) -> None: ...


def clamp_urgency(urgency: float) -> float:
    return max(0.0, min(1.0, float(urgency)))


class PrintSpeaker:
    def __init__(self, delay_per_char_s: float = 0.0, stream=sys.stdout) -> None:  # noqa: ANN001
        self.delay_per_char_s = delay_per_char_s
        self.stream = stream
        self.spoken: list[str] = []
        self.urgencies: list[float] = []

    async def say(self, text: str, urgency: float = 0.0) -> None:
        self.spoken.append(text)
        self.urgencies.append(clamp_urgency(urgency))
        tag = f" (urgency {urgency:.1f})" if urgency > 0 else ""
        print(f"[robot says]{tag} {text}", file=self.stream, flush=True)
        if self.delay_per_char_s > 0:
            await asyncio.sleep(self.delay_per_char_s * len(text))


def espeak_args(voice: str, speed_wpm: int, text: str, urgency: float = 0.0) -> list[str]:
    """espeak-ng arguments for a given urgency: amplitude 100 -> 200, rate +40 %."""
    u = clamp_urgency(urgency)
    amp = int(round(100 + 100 * u))
    rate = int(round(speed_wpm * (1.0 + 0.4 * u)))
    return ["-v", voice, "-s", str(rate), "-a", str(amp), text]


class EspeakSpeaker:
    def __init__(self, voice: str = "en", speed_wpm: int = 165) -> None:
        self.binary = shutil.which("espeak-ng") or shutil.which("espeak")
        if not self.binary:
            raise RuntimeError("espeak-ng not installed")
        self.voice = voice
        self.speed_wpm = speed_wpm

    async def say(self, text: str, urgency: float = 0.0) -> None:
        proc = await asyncio.create_subprocess_exec(
            self.binary, *espeak_args(self.voice, self.speed_wpm, text, urgency)
        )
        await proc.wait()


def elevenlabs_voice_settings(urgency: float = 0.0) -> dict[str, float] | None:
    """Lower stability = more expressive/insistent delivery. None = the voice's defaults."""
    u = clamp_urgency(urgency)
    if u <= 0:
        return None
    return {
        "stability": round(0.5 - 0.4 * u, 2),
        "similarity_boost": 0.75,
        "style": round(0.6 * u, 2),
        "use_speaker_boost": True,
    }


class ElevenLabsSpeaker:
    SAMPLE_RATE = 22050

    def __init__(self, api_key: str, voice_id: str, model_id: str = "eleven_flash_v2_5") -> None:
        from elevenlabs.client import ElevenLabs  # type: ignore[import-not-found]

        self._client = ElevenLabs(api_key=api_key)
        self.voice_id = voice_id
        self.model_id = model_id

    def _say_blocking(self, text: str, urgency: float = 0.0) -> None:
        import sounddevice as sd  # type: ignore[import-not-found]

        kwargs: dict[str, Any] = {}
        vs = elevenlabs_voice_settings(urgency)
        if vs is not None:
            try:
                from elevenlabs import VoiceSettings  # type: ignore[import-not-found]

                kwargs["voice_settings"] = VoiceSettings(**vs)
            except Exception:  # older/newer SDK: the API accepts the plain dict too
                kwargs["voice_settings"] = vs
        stream = self._client.text_to_speech.stream(
            text=text,
            voice_id=self.voice_id,
            model_id=self.model_id,
            output_format="pcm_22050",
            **kwargs,
        )
        with sd.RawOutputStream(
            samplerate=self.SAMPLE_RATE, channels=1, dtype="int16", blocksize=2048
        ) as out:
            carry = b""
            for chunk in stream:
                if not chunk:
                    continue
                buf = carry + chunk
                usable = len(buf) - (len(buf) % 2)
                out.write(buf[:usable])
                carry = buf[usable:]

    async def say(self, text: str, urgency: float = 0.0) -> None:
        try:
            await asyncio.to_thread(self._say_blocking, text, urgency)
        except Exception as exc:  # network/audio failure must not kill the service
            log.error("elevenlabs tts failed: %s", exc)
            # not the text: a spoken line may carry an address, an amount, a message to sign
            print(f"[robot says] ({len(text)} chars, not shown)", flush=True)


def make_speaker(cfg: Settings, *, mock: bool = False) -> Speaker:
    backend = "print" if mock and cfg.tts_backend == "auto" else cfg.tts_backend
    if backend == "elevenlabs" or (backend == "auto" and cfg.elevenlabs_api_key):
        if not cfg.elevenlabs_api_key or not cfg.elevenlabs_voice_id:
            raise RuntimeError("ELEVENLABS_API_KEY and ELEVENLABS_VOICE_ID are required")
        try:
            return ElevenLabsSpeaker(
                cfg.elevenlabs_api_key, cfg.elevenlabs_voice_id, cfg.elevenlabs_model
            )
        except ImportError as exc:
            if backend == "elevenlabs":
                raise
            log.warning("elevenlabs not installed (%s); falling back", exc)
    if backend in ("espeak", "auto"):
        try:
            return EspeakSpeaker()
        except RuntimeError:
            if backend == "espeak":
                raise
            log.warning("espeak-ng not found; printing speech instead")
    return PrintSpeaker()


# --- microphone ---------------------------------------------------------------------------


class Microphone(Protocol):
    sample_rate: int

    def record(self, seconds: float) -> np.ndarray: ...


class MockMicrophone:
    """Returns silence (or a provided waveform) so the voice loop can run without audio."""

    def __init__(self, sample_rate: int = 16000, audio: np.ndarray | None = None) -> None:
        self.sample_rate = sample_rate
        self.audio = audio
        self.calls = 0

    def record(self, seconds: float) -> np.ndarray:
        self.calls += 1
        n = int(seconds * self.sample_rate)
        if self.audio is not None:
            return np.resize(self.audio, n).astype(np.float32)
        return np.zeros(n, dtype=np.float32)


class SoundDeviceMicrophone:
    def __init__(self, sample_rate: int = 16000, device: str | None = None) -> None:
        import sounddevice as sd  # type: ignore[import-not-found]

        self._sd = sd
        self.sample_rate = sample_rate
        self.device = device
        self._lock = threading.Lock()

    def record(self, seconds: float) -> np.ndarray:
        with self._lock:
            frames = int(seconds * self.sample_rate)
            data = self._sd.rec(
                frames,
                samplerate=self.sample_rate,
                channels=1,
                dtype="float32",
                device=self.device,
            )
            self._sd.wait()
        return np.asarray(data, dtype=np.float32).reshape(-1)


def make_microphone(cfg: Settings, *, mock: bool = False) -> Microphone:
    if mock:
        return MockMicrophone(cfg.mic_sample_rate)
    try:
        return SoundDeviceMicrophone(cfg.mic_sample_rate, cfg.mic_device)
    except ImportError:
        log.warning("sounddevice not installed; using a silent mock microphone")
        return MockMicrophone(cfg.mic_sample_rate)
