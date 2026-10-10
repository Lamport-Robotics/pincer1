"""Compute backend selection: CUDA on the Jetson Orin Nano, CPU on a Pi or a laptop.

``PINCER_DEVICE=auto|cuda|cpu`` (``Settings.device``) is resolved once into a
``ComputePlan`` that every heavy backend reads from:

* insightface  -> onnxruntime providers (``CUDAExecutionProvider`` first on cuda) and
  the model pack (``buffalo_l`` on cuda, ``buffalo_sc`` on cpu).
* faster-whisper -> ``device``/``compute_type`` (``cuda``/``float16`` vs ``cpu``/``int8``)
  and the model size (``small`` on cuda, ``base`` on cpu).
* resemblyzer  -> torch device (``cuda`` when torch sees a GPU, else ``cpu``).

Detection never imports anything at module import time and tolerates every heavy
package being absent, so ``--mock`` runs and the test-suite work on a bare venv.
On the Jetson, ``onnxruntime-gpu``, ``torch`` and a CUDA build of ``ctranslate2`` come
from NVIDIA's Jetson wheel index, not PyPI (see pyproject ``[jetson]`` and the README).
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

log = logging.getLogger("pincer.compute")

Device = Literal["cuda", "cpu"]
DeviceSetting = Literal["auto", "cuda", "cpu"]

CUDA_PROVIDERS = ("CUDAExecutionProvider", "CPUExecutionProvider")
CPU_PROVIDERS = ("CPUExecutionProvider",)

FACE_MODEL_BY_DEVICE: dict[Device, str] = {"cuda": "buffalo_l", "cpu": "buffalo_sc"}
WHISPER_MODEL_BY_DEVICE: dict[Device, str] = {"cuda": "small", "cpu": "base"}
WHISPER_COMPUTE_BY_DEVICE: dict[Device, str] = {"cuda": "float16", "cpu": "int8"}


def is_jetson() -> bool:
    """True on an NVIDIA Jetson (L4T) image. Cheap file checks only."""
    if Path("/etc/nv_tegra_release").exists():
        return True
    try:
        model = Path("/proc/device-tree/model").read_bytes()
    except OSError:
        return False
    return b"NVIDIA" in model or b"Jetson" in model


def _onnxruntime_has_cuda() -> bool:
    try:
        import onnxruntime as ort  # type: ignore[import-not-found]
    except Exception:
        return False
    try:
        return "CUDAExecutionProvider" in ort.get_available_providers()
    except Exception:
        return False


def _torch_has_cuda() -> bool:
    try:
        import torch  # type: ignore[import-not-found]
    except Exception:
        return False
    try:
        return bool(torch.cuda.is_available())
    except Exception:
        return False


def _ctranslate2_has_cuda() -> bool:
    try:
        import ctranslate2  # type: ignore[import-not-found]
    except Exception:
        return False
    try:
        return int(ctranslate2.get_cuda_device_count()) > 0
    except Exception:
        return False


def cuda_available() -> bool:
    """Any of the three runtimes we use reports a usable CUDA device.

    ``PINCER_FORCE_NO_CUDA=1`` short-circuits to False (handy for A/B latency checks on
    the Jetson without reinstalling wheels).
    """
    if os.environ.get("PINCER_FORCE_NO_CUDA", "") not in ("", "0", "false", "no"):
        return False
    return _onnxruntime_has_cuda() or _torch_has_cuda() or _ctranslate2_has_cuda()


def resolve_device(setting: str, *, probe=cuda_available) -> Device:  # noqa: ANN001
    """``auto`` -> cuda if any runtime sees a GPU, else cpu. ``cuda`` is honoured even if
    the probe fails (the backends then raise a clear error at construction instead of
    silently running slow); ``cpu`` is always cpu."""
    s = (setting or "auto").strip().lower()
    if s == "cpu":
        return "cpu"
    if s == "cuda":
        if not probe():
            log.warning("PINCER_DEVICE=cuda but no CUDA runtime was found; expect backend errors")
        return "cuda"
    if s != "auto":
        log.warning("unknown PINCER_DEVICE=%r, treating as auto", setting)
    return "cuda" if probe() else "cpu"


@dataclass(frozen=True)
class ComputePlan:
    device: Device
    ort_providers: tuple[str, ...]
    face_model: str
    face_ctx_id: int
    whisper_model: str
    whisper_device: str
    whisper_compute_type: str
    torch_device: str

    @property
    def is_cuda(self) -> bool:
        return self.device == "cuda"

    def describe(self) -> str:
        return (
            f"device={self.device} face={self.face_model} ort={'/'.join(self.ort_providers)} "
            f"whisper={self.whisper_model}@{self.whisper_device}/{self.whisper_compute_type} "
            f"speaker=torch:{self.torch_device}"
        )


def build_plan(
    device: Device,
    *,
    face_model: str | None = None,
    whisper_model: str | None = None,
    whisper_compute_type: str | None = None,
    torch_probe=_torch_has_cuda,  # noqa: ANN001
) -> ComputePlan:
    """Fill the per-device defaults; explicit values (from env) always win."""
    cuda = device == "cuda"
    return ComputePlan(
        device=device,
        ort_providers=CUDA_PROVIDERS if cuda else CPU_PROVIDERS,
        face_model=face_model or FACE_MODEL_BY_DEVICE[device],
        face_ctx_id=0 if cuda else -1,
        whisper_model=whisper_model or WHISPER_MODEL_BY_DEVICE[device],
        whisper_device="cuda" if cuda else "cpu",
        whisper_compute_type=whisper_compute_type or WHISPER_COMPUTE_BY_DEVICE[device],
        # resemblyzer only runs on the GPU if *torch* sees it; onnxruntime-gpu alone
        # (a common half-installed Jetson state) must not send torch to a missing device.
        torch_device="cuda" if cuda and torch_probe() else "cpu",
    )
