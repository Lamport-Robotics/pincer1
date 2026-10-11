"""Training config loading, validation and the step plan, without torch."""

from __future__ import annotations

import json

import pytest
import yaml

from pincer_brain.train.common import (
    CONFIG_DIR,
    ConfigError,
    estimate_jsonl,
    load_config,
    make_plan,
)

ALL = sorted(CONFIG_DIR.glob("*.yaml"))


def test_six_configs_exist():
    names = {p.stem for p in ALL}
    assert names == {
        "cpt_a100_80gb",
        "cpt_h100",
        "sft_a100_80gb",
        "sft_h100",
        "dpo_a100_80gb",
        "dpo_h100",
    }


@pytest.mark.parametrize("path", ALL, ids=lambda p: p.stem)
def test_configs_load(path):
    cfg = load_config(path)
    assert cfg.stage == path.stem.split("_")[0]
    assert cfg.lora is not None and cfg.lora.r == 64 and cfg.lora.targets == "all-linear"
    assert cfg.bf16 and cfg.tokens_per_step > 0
    if cfg.stage == "cpt":
        assert 1_000_000_000 <= cfg.token_budget <= 5_000_000_000
    else:
        assert cfg.epochs and cfg.epochs > 0
    if "h100" in path.stem:
        assert cfg.hardware == "h100_80gb" and cfg.attn == "flash_attention_2"


def _write(tmp_path, **over):
    base = yaml.safe_load((CONFIG_DIR / "cpt_a100_80gb.yaml").read_text())
    base.update(over)
    p = tmp_path / "c.yaml"
    p.write_text(yaml.safe_dump(base))
    return p


@pytest.mark.parametrize(
    "over,msg",
    [
        ({"stage": "rlhf"}, "stage"),
        ({"token_budget": 10}, "token_budget"),
        ({"token_budget": 9_000_000_000}, "token_budget"),
        ({"full_parameter": True}, "lora: null"),
        ({"lora": None}, "full_parameter"),
        ({"lora": {"r": 64, "targets": "q_proj"}}, "all-linear"),
        ({"seq_len": 1000}, "seq_len"),
        ({"hardware": "t4"}, "hardware"),
        ({"attn": "eager"}, "attn"),
        ({"stage": "sft", "token_budget": None}, "epochs"),
        ({"unknown_arg": 1, "stage": "xx"}, "stage"),
    ],
)
def test_validation_errors(tmp_path, over, msg):
    with pytest.raises(ConfigError, match=msg):
        load_config(_write(tmp_path, **over))


def test_full_parameter_allowed(tmp_path):
    cfg = load_config(_write(tmp_path, full_parameter=True, lora=None))
    assert cfg.method == "full" and cfg.lora is None


def test_extras_are_kept(tmp_path):
    cfg = load_config(_write(tmp_path, report_to="wandb"))
    assert cfg.extras == {"report_to": "wandb"}


def test_cpt_plan_steps_from_budget():
    cfg = load_config(CONFIG_DIR / "cpt_a100_80gb.yaml")
    plan = make_plan(cfg, dataset_tokens=400_000_000, dataset_rows=100_000)
    assert plan.tokens_per_step == 4 * 8 * 4096
    assert plan.steps == cfg.token_budget // plan.tokens_per_step
    assert plan.epochs == pytest.approx(5.0)
    assert any("epochs" in n for n in plan.notes)
    assert plan.est_hours > 0
    h100 = make_plan(load_config(CONFIG_DIR / "cpt_h100.yaml"), 400_000_000, 100_000)
    assert h100.est_hours < plan.est_hours


def test_sft_plan_from_rows(tmp_path):
    cfg = load_config(CONFIG_DIR / "sft_a100_80gb.yaml")
    plan = make_plan(cfg, dataset_tokens=1_000_000, dataset_rows=2000)
    assert plan.steps == round(2000 / (cfg.micro_batch * cfg.grad_accum) * cfg.epochs)
    empty = make_plan(cfg, None, None)
    assert empty.steps == 0 and empty.notes


def test_estimate_jsonl(tmp_path):
    p = tmp_path / "d.jsonl"
    rows = [{"text": "a" * 400}, {"text": "b" * 800}]
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    assert estimate_jsonl(p) == (2, 300)
    assert estimate_jsonl(p, fields=("text",)) == (2, 300)
    assert estimate_jsonl(tmp_path / "missing.jsonl") == (0, 0)
