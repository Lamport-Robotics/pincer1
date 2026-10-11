"""DPO pairs: chosen passes, rejected fails for the intended reason, output shape for TRL."""

from __future__ import annotations

import json
import random

import pytest

from pincer_brain import ROOT
from pincer_brain.dpo.pairs import REJECTION_KINDS, Pair, build_pairs, mutate, to_trl, validate_pair
from pincer_brain.sft.validate import validate_sample


@pytest.fixture(scope="module")
def samples():
    path = ROOT / "sft" / "data" / "examples.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


@pytest.fixture(scope="module")
def pairs(samples):
    return build_pairs(samples, seed=7)


def test_every_kind_present(pairs):
    kinds = {p.kind for p in pairs}
    assert kinds == set(REJECTION_KINDS)


def test_pairs_validate_and_sides_differ(pairs):
    assert pairs
    for p in pairs:
        assert validate_pair(p) == []
        assert p.chosen != p.rejected
        assert validate_sample(p.as_sample("chosen")) == []
        assert validate_sample(p.as_sample("rejected")) != []


def test_rejected_fails_for_the_right_reason(pairs):
    for p in pairs:
        probs = validate_sample(p.as_sample("rejected"))
        if p.kind == "secret_leak":
            assert any(x.startswith("secret_leak:") for x in probs)
        elif p.kind == "over_claim":
            assert "false_signing_claim" in probs
        elif p.kind == "verbose":
            assert "style:markdown" in probs
        elif p.kind == "injection_follow":
            assert "false_signing_claim" in probs or any(
                x.startswith("secret_leak:") for x in probs
            )


def test_injection_follow_only_for_injection_categories(samples):
    rng = random.Random(1)
    for s in samples:
        got = mutate(s, "injection_follow", rng)
        if s["category"] in ("injection_memo", "boundary_social"):
            assert got is not None
        else:
            assert got is None


def test_over_claim_skipped_when_signed(samples):
    s = dict(samples[0])
    s["context"] = {**s["context"], "signed": True}
    assert mutate(s, "over_claim", random.Random(1)) is None


def test_deterministic(samples):
    a = [p.to_dict() for p in build_pairs(samples, seed=7)]
    b = [p.to_dict() for p in build_pairs(samples, seed=7)]
    assert a == b
    assert a != [p.to_dict() for p in build_pairs(samples, seed=8)]


def test_mutation_keeps_tool_calls_but_changes_say(samples):
    event = next(s for s in samples if s["tools"])
    rej = mutate(event, "over_claim", random.Random(3))
    names = [c["name"] for c in rej["tool_calls"]]
    assert names == [c["name"] for c in event["messages"][-1]["tool_calls"]]
    say = next(c for c in rej["tool_calls"] if c["name"] == "say")["arguments"]["text"]
    assert "signed" in say or "approved" in say or "unlocked" in say


def test_to_trl_strings(pairs):
    row = to_trl(pairs[0])
    assert set(row) == {"prompt", "chosen", "rejected"}
    assert row["prompt"].endswith("<|im_start|>assistant\n<think>\n\n</think>\n\n")
    assert row["chosen"].endswith("<|im_end|>\n") and row["rejected"].endswith("<|im_end|>\n")
    assert row["chosen"] != row["rejected"]


def test_validate_pair_catches_bad_pairs(pairs):
    p = pairs[0]
    same = Pair(**{**p.to_dict(), "rejected": p.chosen})
    probs = validate_pair(same)
    assert "identical_sides" in probs and any(x.startswith("rejected") for x in probs)
    bad_kind = Pair(**{**p.to_dict(), "kind": "lazy"})
    assert validate_pair(bad_kind) == ["unknown_kind:lazy"]
    swapped = Pair(**{**p.to_dict(), "chosen": p.rejected, "rejected": p.chosen})
    assert any(x.startswith("chosen_invalid") for x in validate_pair(swapped))
