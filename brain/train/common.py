"""Config loading and the training plan, with no torch import.

A config is one YAML file per stage and GPU (``train/configs/*.yaml``). ``load_config``
validates it; ``make_plan`` turns a token budget or an epoch count into a step count and a
rough wall-clock estimate so the owner can read the cost of a run before starting it.
"""

from __future__ import annotations

import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import yaml

STAGES = ("cpt", "sft", "dpo")
HERE = Path(__file__).resolve().parent
CONFIG_DIR = HERE / "configs"
CHARS_PER_TOKEN = 4

#: Rough training throughput, tokens/second, for an 8B model in bf16 with LoRA r=64 and
#: gradient checkpointing at 4k context. Conservative; used only for the printed estimate.
THROUGHPUT_TOK_S = {
    ("a100_80gb", "lora"): 2600,
    ("a100_80gb", "full"): 1500,
    ("h100_80gb", "lora"): 5200,
    ("h100_80gb", "full"): 3000,
}

CPT_BUDGET_MIN = 1_000_000_000
CPT_BUDGET_MAX = 5_000_000_000


@dataclass
class LoraSpec:
    r: int = 64
    alpha: int = 128
    dropout: float = 0.05
    targets: str = "all-linear"


@dataclass
class TrainConfig:
    stage: str
    base_model: str
    output_dir: str
    data_path: str
    hardware: str  # a100_80gb | h100_80gb
    seq_len: int = 4096
    micro_batch: int = 4
    grad_accum: int = 8
    lr: float = 1e-4
    warmup_ratio: float = 0.03
    weight_decay: float = 0.0
    epochs: float | None = None
    token_budget: int | None = None
    lora: LoraSpec | None = field(default_factory=LoraSpec)
    full_parameter: bool = False
    quant: str | None = None  # nf4 (bitsandbytes, Linux/CUDA only)
    packing: bool = True
    bf16: bool = True
    gradient_checkpointing: bool = True
    attn: str = "sdpa"  # sdpa | flash_attention_2
    think_tags: bool = True
    dpo_beta: float = 0.1
    dpo_loss: str = "sigmoid"
    save_steps: int = 200
    logging_steps: int = 10
    eval_fraction: float = 0.02
    seed: int = 7
    text_field: str = "text"
    extras: dict[str, Any] = field(default_factory=dict)

    @property
    def tokens_per_step(self) -> int:
        return self.micro_batch * self.grad_accum * self.seq_len

    @property
    def method(self) -> str:
        return "full" if self.full_parameter else "lora"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class ConfigError(ValueError):
    pass


def load_config(path: Path | str) -> TrainConfig:
    raw = yaml.safe_load(Path(path).read_text())
    if not isinstance(raw, dict):
        raise ConfigError("config must be a mapping")
    known = {f for f in TrainConfig.__dataclass_fields__}
    lora_raw = raw.pop("lora", {})
    extras = {k: raw.pop(k) for k in list(raw) if k not in known}
    try:
        cfg = TrainConfig(**raw, extras=extras)
    except TypeError as exc:
        raise ConfigError(str(exc)) from exc
    if lora_raw is None or lora_raw is False:
        cfg.lora = None
    elif isinstance(lora_raw, dict):
        cfg.lora = LoraSpec(**lora_raw)
    else:
        raise ConfigError("lora must be a mapping, null or false")
    validate_config(cfg)
    return cfg


def validate_config(cfg: TrainConfig) -> None:
    if cfg.stage not in STAGES:
        raise ConfigError(f"stage must be one of {STAGES}, got {cfg.stage!r}")
    if cfg.hardware not in {h for h, _ in THROUGHPUT_TOK_S}:
        raise ConfigError(f"unknown hardware {cfg.hardware!r}")
    if cfg.full_parameter and cfg.lora is not None:
        raise ConfigError("full_parameter: true needs lora: null")
    if not cfg.full_parameter and cfg.lora is None:
        raise ConfigError("either full_parameter: true or a lora block")
    if cfg.lora and cfg.lora.targets != "all-linear":
        raise ConfigError("lora.targets must be 'all-linear' (every linear layer)")
    if cfg.seq_len < 512 or cfg.seq_len % 64:
        raise ConfigError("seq_len must be >= 512 and a multiple of 64")
    if cfg.micro_batch < 1 or cfg.grad_accum < 1:
        raise ConfigError("micro_batch and grad_accum must be >= 1")
    if cfg.stage == "cpt":
        if cfg.token_budget is None:
            raise ConfigError("cpt needs token_budget")
        if not CPT_BUDGET_MIN <= cfg.token_budget <= CPT_BUDGET_MAX:
            raise ConfigError(f"token_budget must be within {CPT_BUDGET_MIN:,}..{CPT_BUDGET_MAX:,}")
    else:
        if cfg.epochs is None or cfg.epochs <= 0:
            raise ConfigError(f"{cfg.stage} needs epochs > 0")
    if cfg.quant not in (None, "nf4"):
        raise ConfigError("quant must be null or nf4")
    if cfg.quant == "nf4" and sys.platform != "linux":
        raise ConfigError("quant: nf4 needs bitsandbytes, Linux/CUDA only")
    if cfg.attn not in ("sdpa", "flash_attention_2"):
        raise ConfigError("attn must be sdpa or flash_attention_2")


@dataclass
class Plan:
    stage: str
    method: str
    tokens_per_step: int
    dataset_tokens: int | None
    dataset_rows: int | None
    steps: int
    epochs: float
    est_hours: float
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def estimate_jsonl(
    path: Path | str, text_field: str = "text", fields: tuple[str, ...] = ()
) -> tuple[int, int]:
    """``(rows, est_tokens)`` for a JSONL file without a tokenizer (chars / 4)."""
    rows = 0
    chars = 0
    p = Path(path)
    if not p.exists():
        return 0, 0
    with p.open() as f:
        for line in f:
            if not line.strip():
                continue
            rows += 1
            row = json.loads(line)
            if fields:
                chars += sum(len(str(row.get(k, ""))) for k in fields)
            elif text_field in row:
                chars += len(str(row[text_field]))
            else:
                chars += len(line)
    return rows, chars // CHARS_PER_TOKEN


def make_plan(
    cfg: TrainConfig, dataset_tokens: int | None, dataset_rows: int | None = None
) -> Plan:
    tps = cfg.tokens_per_step
    notes: list[str] = []
    if cfg.stage == "cpt":
        assert cfg.token_budget is not None
        steps = max(1, cfg.token_budget // tps)
        epochs = (cfg.token_budget / dataset_tokens) if dataset_tokens else 0.0
        if dataset_tokens and epochs > 4:
            notes.append(
                f"budget is {epochs:.1f} epochs over the corpus; more than 4 risks memorisation"
            )
        if dataset_tokens and dataset_tokens < CPT_BUDGET_MIN / 4:
            notes.append(
                "corpus is small for continued pretraining; add sources before spending the budget"
            )
        trained_tokens = steps * tps
    else:
        assert cfg.epochs is not None
        if dataset_rows:
            per_epoch = max(1, round(dataset_rows / (cfg.micro_batch * cfg.grad_accum)))
            steps = max(1, round(per_epoch * cfg.epochs))
        else:
            steps = 0
            notes.append("dataset not found; step count unknown until the data exists")
        epochs = cfg.epochs
        trained_tokens = (dataset_tokens or 0) * cfg.epochs * (3 if cfg.stage == "dpo" else 1)
    rate = THROUGHPUT_TOK_S[(cfg.hardware, cfg.method)]
    est_hours = trained_tokens / rate / 3600 if trained_tokens else 0.0
    if cfg.quant == "nf4":
        notes.append("nf4 quantised base: slower per step, fits where bf16 would not")
    return Plan(
        cfg.stage, cfg.method, tps, dataset_tokens, dataset_rows, steps, epochs, est_hours, notes
    )


def print_plan(cfg: TrainConfig, plan: Plan) -> None:
    from rich.console import Console
    from rich.table import Table

    t = Table(title=f"{cfg.stage} plan ({cfg.hardware}, {plan.method})", show_header=False)
    t.add_column("k")
    t.add_column("v")
    t.add_row("base model", cfg.base_model)
    t.add_row("data", cfg.data_path)
    t.add_row("output", cfg.output_dir)
    t.add_row(
        "seq_len x micro x accum",
        f"{cfg.seq_len} x {cfg.micro_batch} x {cfg.grad_accum} = {plan.tokens_per_step:,} tok/step",
    )
    if plan.dataset_rows is not None:
        t.add_row("dataset", f"{plan.dataset_rows:,} rows, ~{plan.dataset_tokens or 0:,} tokens")
    if cfg.token_budget:
        t.add_row("token budget", f"{cfg.token_budget:,}")
    t.add_row("steps", f"{plan.steps:,}")
    t.add_row("epochs", f"{plan.epochs:.2f}")
    t.add_row("lr / warmup", f"{cfg.lr} / {cfg.warmup_ratio}")
    if cfg.lora:
        t.add_row(
            "lora",
            f"r={cfg.lora.r} alpha={cfg.lora.alpha} dropout={cfg.lora.dropout} "
            f"targets={cfg.lora.targets}",
        )
    t.add_row("est. wall clock", f"{plan.est_hours:.1f} h")
    for n in plan.notes:
        t.add_row("note", n)
    Console(stderr=True, width=120).print(t)


def common_args(ap: Any, default_config: str) -> None:
    ap.add_argument("--config", type=Path, default=CONFIG_DIR / default_config)
    ap.add_argument("--data", type=Path, default=None, help="override data_path")
    ap.add_argument("--output", type=Path, default=None, help="override output_dir")
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="print the plan and exit (default when --run is absent)",
    )
    ap.add_argument(
        "--run", action="store_true", help="actually train: loads the model, uses the GPU"
    )


def resolve(args: Any) -> TrainConfig:
    cfg = load_config(args.config)
    if args.data:
        cfg.data_path = str(args.data)
    if args.output:
        cfg.output_dir = str(args.output)
    return cfg


def lora_config(cfg: TrainConfig) -> Any:
    """peft.LoraConfig for the stage (imported lazily)."""
    from peft import LoraConfig

    assert cfg.lora is not None
    return LoraConfig(
        r=cfg.lora.r,
        lora_alpha=cfg.lora.alpha,
        lora_dropout=cfg.lora.dropout,
        target_modules=cfg.lora.targets,
        task_type="CAUSAL_LM",
    )


def load_model_and_tokenizer(cfg: TrainConfig) -> tuple[Any, Any]:
    """bf16 model + tokenizer, optional nf4. Imported lazily; only called under ``--run``."""
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    tok = AutoTokenizer.from_pretrained(cfg.base_model)
    if tok.pad_token is None:
        tok.pad_token = tok.eos_token
    kwargs: dict[str, Any] = {
        "torch_dtype": torch.bfloat16 if cfg.bf16 else torch.float32,
        "attn_implementation": cfg.attn,
    }
    if cfg.quant == "nf4":
        from transformers import BitsAndBytesConfig

        kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_compute_dtype=torch.bfloat16,
            bnb_4bit_use_double_quant=True,
        )
    model = AutoModelForCausalLM.from_pretrained(cfg.base_model, **kwargs)
    if cfg.gradient_checkpointing:
        model.gradient_checkpointing_enable()
        model.config.use_cache = False
    return model, tok


def write_provenance(cfg: TrainConfig, plan: Plan, extra: dict[str, Any] | None = None) -> None:
    out = Path(cfg.output_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "train_config.json").write_text(
        json.dumps({"config": cfg.to_dict(), "plan": plan.to_dict(), **(extra or {})}, indent=1)
    )
