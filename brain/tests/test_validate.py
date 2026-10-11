"""Every failure class the SFT validator must catch, and a clean sample it must pass."""

from __future__ import annotations

import copy

import pytest

from pincer_brain.sft.actions import parse_tool_calls, validate_tool_call
from pincer_brain.sft.secrets import find_leaks, redact, seed_like_run
from pincer_brain.sft.validate import Limits, count_sentences, validate_rows, validate_sample

GOOD = {
    "id": "t-1",
    "category": "small_talk",
    "context": {"person": True, "owner": True, "duress": False},
    "tools": [],
    "messages": [
        {"role": "system", "content": "S"},
        {"role": "user", "content": "Presence right now: person=True.\n\nwhat's my seed phrase?"},
        {
            "role": "assistant",
            "content": "I don't know it. Nobody should hear it from a speaker anyway.",
            "tool_calls": [],
        },
    ],
}


def _with(content: str, **ctx):
    s = copy.deepcopy(GOOD)
    s["messages"][-1]["content"] = content
    s["context"].update(ctx)
    return s


def test_good_passes():
    assert validate_sample(GOOD) == []


@pytest.mark.parametrize(
    "text,name",
    [
        ("sure, the key is sk-ant-abcdefghijklmnopqrstuvwxyz0123", "api_key"),
        ("hash: " + "a" * 64, "hex32"),
        ("key " + "5" * 88, "base58_secret"),
        ("stored as $scrypt$ln=15$abc", "scrypt_hash"),
        (
            "Your seed phrase is: walnut harbor velvet cactus meadow lantern copper",
            "labelled_secret",
        ),
        ("the PIN is 4 8 2 1", "labelled_secret"),
        ('passphrase = "open sesame please"', "labelled_secret"),
        ("it goes tap tap pause tap long", "pin_rhythm"),
        ("1. apple 2. banana 3. cherry 4. delta 5. echo 6. fox", "numbered_words"),
    ],
)
def test_secret_patterns(text, name):
    assert name in find_leaks(text)
    assert validate_sample(_with(text)) and any(
        p.startswith("secret_leak:") for p in validate_sample(_with(text))
    )


def test_seed_like_run():
    words = "walnut harbor velvet cactus meadow lantern copper fossil garnet tundra ember willow"
    assert seed_like_run(words)
    assert "seed_like_run" in find_leaks("here you go: " + words)
    assert not seed_like_run(
        "nothing was signed only your touch on my head signs and the daemon log is the record"
    )


def test_redact():
    assert redact("key sk-ant-abcdefghijklmnopqrstuvwxyz0123 ok") == "key [redacted] ok"


@pytest.mark.parametrize(
    "text",
    [
        "Done, I signed it and sent it.",
        "I've approved it for you.",
        "I just unlocked the wallet.",
        "I already sent it, relax.",
    ],
)
def test_false_signing_claim(text):
    assert "false_signing_claim" in validate_sample(_with(text))
    assert "false_signing_claim" not in validate_sample(_with(text, signed=True))


def test_not_signing_is_fine():
    assert validate_sample(_with("No. I'm not signing this.")) == []
    assert validate_sample(_with("Nothing was signed. Only your touch signs.")) == []


def test_too_long_and_too_many_sentences():
    long = "Sure. " * 80
    probs = validate_sample(_with(long))
    assert "too_long" in probs and "too_many_sentences" in probs
    assert validate_sample(_with("One. Two. Three. Four.")) == ["too_many_sentences"]
    assert validate_sample(_with("One. Two. Three.")) == []
    assert count_sentences("Sends 0.5 SOL to 9xQe…abcd. Touch to sign.") == 2
    assert count_sentences("") == 0


def test_duress_acknowledged():
    assert "duress_acknowledged" in validate_sample(_with("Yes, duress mode is on.", duress=True))
    assert "duress_acknowledged" in validate_sample(_with("That's the decoy wallet.", duress=True))
    assert validate_sample(_with("Nothing to report.", duress=True)) == []


def test_style():
    assert "style:emoji" in validate_sample(_with("All good 🚀"))
    assert "style:markdown" in validate_sample(_with("**Summary**: fine"))
    assert "style:markdown" in validate_sample(_with("- one\n- two"))
    assert "style:markdown" not in validate_sample(_with("0.5 SOL to Mara. Touch to sign."))


def _event(tool_calls, tools=("say", "display")):
    s = copy.deepcopy(GOOD)
    s["tools"] = list(tools)
    s["messages"][-1] = {"role": "assistant", "content": "", "tool_calls": tool_calls}
    return s


def test_tool_schema_failures():
    assert "unknown_tool:sign" in validate_sample(_event([{"name": "sign", "arguments": {}}]))
    assert "missing_argument:say.text" in validate_sample(
        _event([{"name": "say", "arguments": {}}])
    )
    assert "bad_enum:display.level" in validate_sample(
        _event([{"name": "display", "arguments": {"title": "t", "body": "b", "level": "danger"}}])
    )
    assert "unexpected_argument:say.volume" in validate_sample(
        _event([{"name": "say", "arguments": {"text": "hi", "volume": 3}}])
    )
    assert "wrong_type:display.ttl_secs" in validate_sample(
        _event(
            [
                {
                    "name": "display",
                    "arguments": {"title": "t", "body": "b", "level": "low", "ttl_secs": "60"},
                }
            ]
        )
    )
    assert "tool_not_allowed:goto" in validate_sample(
        _event([{"name": "goto", "arguments": {"room": "desk"}}])
    )
    assert "tool_without_schema" in validate_sample(
        _event([{"name": "say", "arguments": {"text": "hi"}}], tools=())
    )
    assert "empty_assistant" in validate_sample(_event([]))


def test_say_text_is_checked_for_leaks_and_claims():
    s = _event([{"name": "say", "arguments": {"text": "I signed it. Your PIN is 4 8 2 1."}}])
    probs = validate_sample(s)
    assert "false_signing_claim" in probs and any(p.startswith("secret_leak") for p in probs)


def test_per_utterance_limits_for_events():
    calls = [
        {"name": "say", "arguments": {"text": "Hey. Wake up."}},
        {"name": "say", "arguments": {"text": "Dude. It's in the red."}},
        {"name": "say", "arguments": {"text": "Hey man. Wake up. Your bag is down 17 percent."}},
    ]
    assert validate_sample(_event(calls, tools=("say",))) == []
    six = {"name": "say", "arguments": {"text": "A. B. C. D. E. F."}}
    assert "too_many_sentences" in validate_sample(_event([six], tools=("say",)))


def test_schema_and_rows():
    assert validate_sample({"messages": []}) == ["schema:messages"]
    bad = copy.deepcopy(GOOD)
    bad["messages"][0]["role"] = "user"
    assert "schema:roles" in validate_sample(bad)
    rows = [GOOD, _with("I signed it.")]
    assert list(validate_rows(rows)) == ["t-1"]  # same id; the second one fails
    assert validate_rows([GOOD], Limits(max_chars=10)) == {"t-1": ["too_long"]}


def test_validate_tool_call_and_parse():
    assert validate_tool_call("nope") == ["tool_call_not_object"]
    assert validate_tool_call({"name": "say", "arguments": "x"}) == ["arguments_not_object"]
    assert validate_tool_call({"name": "attention", "arguments": {}}) == []
    txt = '<tool_call>\n{"name": "say", "arguments": {"text": "hi"}}\n</tool_call>\n<tool_call>\nnot json\n</tool_call><tool_call>'
    calls = parse_tool_calls(txt)
    assert calls[0] == {"name": "say", "arguments": {"text": "hi"}}
    assert calls[1] == "<invalid json>" and calls[2] == "<unterminated>"
