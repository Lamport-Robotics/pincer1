import sys

from pincer_robot import compute
from pincer_robot.compute import ComputePlan, build_plan, cuda_available, resolve_device
from pincer_robot.config import Settings


def test_auto_picks_cuda_when_probe_true() -> None:
    assert resolve_device("auto", probe=lambda: True) == "cuda"
    assert resolve_device("auto", probe=lambda: False) == "cpu"
    assert resolve_device("", probe=lambda: True) == "cuda"


def test_explicit_cpu_and_cuda_are_honoured() -> None:
    assert resolve_device("cpu", probe=lambda: True) == "cpu"
    assert resolve_device("CUDA", probe=lambda: False) == "cuda"  # warns, does not downgrade
    assert resolve_device("tpu", probe=lambda: False) == "cpu"  # unknown -> auto


def test_plan_defaults_per_device() -> None:
    cuda = build_plan("cuda", torch_probe=lambda: True)
    assert cuda.ort_providers == ("CUDAExecutionProvider", "CPUExecutionProvider")
    assert cuda.face_model == "buffalo_l" and cuda.face_ctx_id == 0
    assert (cuda.whisper_model, cuda.whisper_device, cuda.whisper_compute_type) == (
        "small", "cuda", "float16",
    )  # fmt: skip
    assert cuda.torch_device == "cuda" and cuda.is_cuda

    cpu = build_plan("cpu")
    assert cpu.ort_providers == ("CPUExecutionProvider",)
    assert cpu.face_model == "buffalo_sc" and cpu.face_ctx_id == -1
    assert (cpu.whisper_model, cpu.whisper_device, cpu.whisper_compute_type) == (
        "base", "cpu", "int8",
    )  # fmt: skip
    assert cpu.torch_device == "cpu" and not cpu.is_cuda


def test_cuda_without_torch_keeps_resemblyzer_on_cpu() -> None:
    # onnxruntime-gpu installed but torch not (half-installed Jetson): speaker model stays on CPU.
    plan = build_plan("cuda", torch_probe=lambda: False)
    assert plan.whisper_device == "cuda" and plan.torch_device == "cpu"


def test_explicit_model_overrides_win() -> None:
    plan = build_plan(
        "cpu", face_model="buffalo_l", whisper_model="tiny", whisper_compute_type="int8_float16"
    )
    assert plan.face_model == "buffalo_l"
    assert plan.whisper_model == "tiny" and plan.whisper_compute_type == "int8_float16"
    assert "buffalo_l" in plan.describe() and "tiny@cpu/int8_float16" in plan.describe()


def test_probe_is_false_without_any_runtime(monkeypatch) -> None:  # noqa: ANN001
    # Make every heavy import fail, as on a bare laptop venv.
    for name in ("onnxruntime", "torch", "ctranslate2"):
        monkeypatch.setitem(sys.modules, name, None)
    monkeypatch.delenv("PINCER_FORCE_NO_CUDA", raising=False)
    assert cuda_available() is False


def test_force_no_cuda_env(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.setenv("PINCER_FORCE_NO_CUDA", "1")
    monkeypatch.setattr(compute, "_onnxruntime_has_cuda", lambda: True)
    assert cuda_available() is False


def test_settings_compute_plan_uses_device_setting(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.setattr(compute, "cuda_available", lambda: True)
    cfg = Settings(_env_file=None, device="cpu")  # type: ignore[call-arg]
    assert isinstance(cfg.compute_plan(), ComputePlan)
    assert cfg.compute_plan().device == "cpu"
    monkeypatch.setenv("PINCER_DEVICE", "cuda")
    monkeypatch.setenv("PINCER_WHISPER_MODEL", "medium")
    monkeypatch.setattr(compute, "_torch_has_cuda", lambda: False)
    plan = Settings(_env_file=None).compute_plan()  # type: ignore[call-arg]
    assert plan.device == "cuda" and plan.whisper_model == "medium"
    assert plan.face_model == "buffalo_l"
    assert plan.torch_device == "cpu"


def test_settings_device_default_is_auto(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("PINCER_DEVICE", raising=False)
    cfg = Settings(_env_file=None)  # type: ignore[call-arg]
    assert cfg.device == "auto" and cfg.face_model is None and cfg.whisper_model is None
