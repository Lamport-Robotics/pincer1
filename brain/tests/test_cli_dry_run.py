"""Every CLI has a --dry-run that touches no GPU, no model API and no network."""

from __future__ import annotations

import json
import os

import pytest

from pincer_brain import ROOT
from pincer_brain.corpus import collect
from pincer_brain.dpo import pairs
from pincer_brain.eval import run as eval_run
from pincer_brain.sft import format as fmt
from pincer_brain.sft import generate, validate
from pincer_brain.train import cpt, dpo, merge, sft

EXAMPLES = ROOT / "sft" / "data" / "examples.jsonl"


@pytest.fixture(autouse=True)
def no_api_key(monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    yield
    assert "ANTHROPIC_API_KEY" not in os.environ


def test_generate_dry_run(tmp_path):
    out = tmp_path / "x.jsonl"
    assert generate.main(["--dry-run", "--out", str(out)]) == 0
    assert not out.exists()


def test_generate_examples_matches_committed(tmp_path):
    out = tmp_path / "ex.jsonl"
    assert generate.main(["--examples", "50", "--out", str(out)]) == 0
    assert out.read_text() == EXAMPLES.read_text()


def test_augment_refuses_without_key(tmp_path):
    with pytest.raises(RuntimeError, match="ANTHROPIC_API_KEY"):
        generate.augment_users([], client=None)


def test_augment_with_injected_client():
    class Block:
        type = "text"
        text = json.dumps(
            ["what did I miss overnight?", "anything happen while I slept?", "night summary please"]
        )

    class Resp:
        stop_reason = "end_turn"
        content = [Block()]

    calls = []

    class Client:
        class messages:  # noqa: N801
            @staticmethod
            def create(**kw):
                calls.append(kw)
                return Resp()

    gen = generate.Generator(generate.load_scenarios())
    samples = [gen.small_talk(0), gen.small_talk(1), gen.tx_explain(0)]
    extra = generate.augment_users(samples, fraction=1.0, client=Client())
    assert extra and all(s.id.endswith(("-aug0", "-aug1", "-aug2")) for s in extra)
    assert all(not s.tools for s in extra)
    assert calls and calls[0]["model"] == "claude-sonnet-5-5"
    assert all(
        s.messages[2] == samples[0].messages[2] or s.messages[2] == samples[1].messages[2]
        for s in extra
    )


def test_validate_and_format_dry_run(tmp_path):
    assert validate.main([str(EXAMPLES), "--dry-run"]) == 0
    assert validate.main([str(EXAMPLES)]) == 0
    out = tmp_path / "f.jsonl"
    assert fmt.main([str(EXAMPLES), str(out), "--dry-run"]) == 0 and not out.exists()
    assert fmt.main([str(EXAMPLES), str(out)]) == 0 and out.exists()
    row = json.loads(out.read_text().splitlines()[0])
    assert set(row) == {"prompt", "completion", "text"}


def test_pairs_dry_run(tmp_path):
    out = tmp_path / "p.jsonl"
    assert pairs.main(["--sft", str(EXAMPLES), "--out", str(out), "--dry-run"]) == 0
    assert not out.exists()
    assert pairs.main(["--sft", str(EXAMPLES), "--out", str(out), "--trl"]) == 0
    row = json.loads(out.read_text().splitlines()[0])
    assert set(row) == {"prompt", "chosen", "rejected"}


def test_collect_dry_run(tmp_path):
    out = tmp_path / "c.jsonl"
    assert (
        collect.main(
            [
                "--sources",
                str(ROOT / "corpus" / "fixtures" / "sources.yaml"),
                "--dry-run",
                "--out",
                str(out),
            ]
        )
        == 0
    )
    assert not out.exists()
    assert (
        collect.main(
            ["--sources", str(ROOT / "corpus" / "fixtures" / "sources.yaml"), "--out", str(out)]
        )
        == 0
    )
    assert out.exists() and len(out.read_text().splitlines()) >= 4


@pytest.mark.parametrize(
    "mod,cfg", [(cpt, "cpt_a100_80gb"), (sft, "sft_h100"), (dpo, "dpo_a100_80gb")]
)
def test_train_plan_only_without_run(mod, cfg):
    assert mod.main(["--config", str(ROOT / "train" / "configs" / f"{cfg}.yaml")]) == 0
    assert mod.main(["--config", str(ROOT / "train" / "configs" / f"{cfg}.yaml"), "--dry-run"]) == 0


def test_train_run_without_data_exits_2(tmp_path):
    assert (
        sft.main(
            [
                "--config",
                str(ROOT / "train" / "configs" / "sft_a100_80gb.yaml"),
                "--run",
                "--data",
                str(tmp_path / "none.jsonl"),
            ]
        )
        == 2
    )


def test_merge_dry_run(tmp_path):
    assert (
        merge.main(["--base", "x", "--adapter", str(tmp_path), "--out", str(tmp_path / "o")]) == 0
    )
    assert (
        merge.main(
            ["--base", "x", "--adapter", str(tmp_path), "--out", str(tmp_path / "o"), "--run"]
        )
        == 2
    )


def test_eval_dry_run():
    assert eval_run.main(["--dry-run"]) == 0
    assert eval_run.main([]) == 0  # no source given: prompts only, no scoring
