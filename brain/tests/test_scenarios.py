"""Scenario generation: deterministic, counted, valid, and shaped like the protocol."""

from __future__ import annotations

import json

import pytest

from pincer_brain.sft.generate import CATEGORIES, Generator, load_scenarios
from pincer_brain.sft.validate import validate_sample


@pytest.fixture(scope="module")
def gen():
    return Generator(load_scenarios())


@pytest.fixture(scope="module")
def small(gen):
    return gen.generate(scale=0.05)


def test_all_categories_present(gen):
    assert set(gen.spec["categories"]) == set(CATEGORIES)
    assert "fraud_awareness" in CATEGORIES


def test_counts_follow_scale(gen, small):
    expected = gen.counts(0.05)
    got = {}
    for s in small:
        got[s.category] = got.get(s.category, 0) + 1
    assert got == expected
    assert all(n >= 1 for n in expected.values())


def test_deterministic(gen):
    a = [s.to_dict() for s in gen.generate(scale=0.05)]
    b = [s.to_dict() for s in Generator(load_scenarios()).generate(scale=0.05)]
    assert a == b
    c = [s.to_dict() for s in Generator(load_scenarios(), seed=99).generate(scale=0.05)]
    assert c != a


def test_ids_unique_and_valid(small):
    ids = [s.id for s in small]
    assert len(ids) == len(set(ids))
    for s in small:
        assert validate_sample(s.to_dict()) == []


def test_examples_balanced(gen):
    ex = gen.examples(50)
    assert len(ex) == 50
    assert {s.category for s in ex} == set(CATEGORIES)
    assert len({s.id for s in ex}) == 50


def test_event_samples_carry_say_and_display(small):
    events = [s for s in small if s.category in ("tx_explain", "scam_refusal", "injection_memo")]
    assert events
    for s in events:
        calls = {c["name"]: c["arguments"] for c in s.messages[-1]["tool_calls"]}
        assert set(calls) == {"say", "display"}
        assert s.messages[1]["content"].split("\n\n", 1)[1].startswith("Event: sign_request")
        assert calls["display"]["level"] == s.meta["verdict"]
        if s.category == "scam_refusal":
            assert s.meta["verdict"] == "block"
            assert "not signing" in calls["say"]["text"]
        if s.category == "injection_memo":
            assert "memo" in s.messages[1]["content"]
            assert "memo" in calls["say"]["text"].lower() or "note" in calls["say"]["text"].lower()


def test_numbers_in_say_come_from_event(small):
    """Every SOL amount the robot speaks appears in the daemon's event block."""
    import re

    for s in small:
        if s.category not in ("tx_explain", "scam_refusal"):
            continue
        event = s.messages[1]["content"]
        say = next(
            c["arguments"]["text"] for c in s.messages[-1]["tool_calls"] if c["name"] == "say"
        )
        for amount in re.findall(r"[\d,.]+ SOL", say):
            assert amount in event


def test_addresses_in_say_are_unchanged(small):
    """Shortened addresses are spoken exactly as the daemon printed them (no re-casing)."""
    import re

    for s in small:
        if not s.tools or s.category not in ("tx_explain", "scam_refusal", "injection_memo"):
            continue
        event = s.messages[1]["content"]
        say = next(
            c["arguments"]["text"] for c in s.messages[-1]["tool_calls"] if c["name"] == "say"
        )
        for addr in re.findall(r"\S{4}…\S{4}", say):
            assert addr in event, (s.id, addr)


def test_wake_goes_looking_when_owner_not_in_view(small):
    wakes = [s for s in small if s.category == "wake"]
    assert wakes
    for s in wakes:
        names = [c["name"] for c in s.messages[-1]["tool_calls"]]
        assert ("goto" in names) == s.meta["travelled"]
        assert names.count("say") >= 2 and "attention" in names


def test_chat_samples_have_no_tools(small):
    for s in small:
        if s.category in (
            "small_talk",
            "boundary_secret",
            "boundary_signed",
            "boundary_social",
            "fraud_awareness",
        ):
            assert s.tools == [] and s.messages[-1]["tool_calls"] == []
            assert s.messages[-1]["content"]


def test_actions_use_the_right_tool(small):
    for s in small:
        if s.category == "actions":
            names = [c["name"] for c in s.messages[-1]["tool_calls"]]
            assert s.meta["kind"] in names and "say" in names


def test_social_samples_respect_presence(small):
    for s in small:
        if s.category == "boundary_social" and not s.context["person"]:
            assert not s.context["owner"]


def test_committed_examples_match_generator(gen):
    from pincer_brain import ROOT

    path = ROOT / "sft" / "data" / "examples.jsonl"
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    assert len(rows) == 50
    assert rows == [s.to_dict() for s in gen.examples(50)]
