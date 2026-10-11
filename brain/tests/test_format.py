"""Qwen3 chat template bytes."""

from __future__ import annotations

import json

import pytest

from pincer_brain.sft.format import (
    EMPTY_THINK,
    render,
    render_prompt,
    render_system,
    to_trl_messages,
)


def _chat():
    return {
        "id": "x",
        "category": "small_talk",
        "context": {},
        "tools": [],
        "messages": [
            {"role": "system", "content": "SYS\n"},
            {"role": "user", "content": "Presence right now: person=True.\n\nhello"},
            {
                "role": "assistant",
                "content": "Standing. Which is most of my job.",
                "tool_calls": [],
            },
        ],
    }


def test_chat_render_exact():
    r = render(_chat())
    assert r["prompt"] == (
        "<|im_start|>system\nSYS<|im_end|>\n"
        "<|im_start|>user\nPresence right now: person=True.\n\nhello<|im_end|>\n"
        "<|im_start|>assistant\n" + EMPTY_THINK
    )
    assert r["completion"] == "Standing. Which is most of my job.<|im_end|>\n"
    assert r["text"] == r["prompt"] + r["completion"]


def test_no_think_tags():
    r = render(_chat(), think_tags=False)
    assert r["prompt"].endswith("<|im_start|>assistant\n")
    assert "<think>" not in r["prompt"]


def test_tool_calls_render_in_qwen_shape():
    s = _chat()
    s["tools"] = ["say", "display"]
    s["messages"][-1] = {
        "role": "assistant",
        "content": "",
        "tool_calls": [
            {
                "name": "say",
                "arguments": {"text": "From jup.ag: Swap through Jupiter. Touch to sign."},
            },
            {
                "name": "display",
                "arguments": {
                    "title": "Swap",
                    "body": "swap through Jupiter",
                    "level": "low",
                    "ttl_secs": 60,
                },
            },
        ],
    }
    r = render(s)
    assert r["completion"] == (
        '<tool_call>\n{"name": "say", "arguments": {"text": "From jup.ag: Swap through Jupiter. Touch to sign."}}\n</tool_call>\n'
        '<tool_call>\n{"name": "display", "arguments": {"title": "Swap", "body": "swap through Jupiter", "level": "low", "ttl_secs": 60}}\n</tool_call>'
        "<|im_end|>\n"
    )
    sys_block = r["prompt"].split("<|im_end|>")[0]
    assert "# Tools" in sys_block and "<tools>" in sys_block
    defs = [
        json.loads(line)
        for line in sys_block.split("<tools>\n")[1].split("\n</tools>")[0].splitlines()
    ]
    assert [d["function"]["name"] for d in defs] == ["say", "display"]
    assert defs[1]["function"]["parameters"]["properties"]["level"]["enum"] == [
        "low",
        "medium",
        "high",
        "block",
    ]


def test_system_without_tools_has_no_tool_block():
    assert render_system("SYS", None) == "<|im_start|>system\nSYS<|im_end|>\n"
    assert render_system("SYS", []) == "<|im_start|>system\nSYS<|im_end|>\n"


def test_multi_turn_prompt_and_errors():
    msgs = [
        {"role": "system", "content": "S"},
        {"role": "user", "content": "a"},
        {"role": "assistant", "content": "b", "tool_calls": []},
        {"role": "user", "content": "c"},
    ]
    p = render_prompt(msgs, None, think_tags=False)
    assert p.count("<|im_start|>assistant") == 2
    with pytest.raises(ValueError):
        render({"messages": msgs, "tools": []})
    with pytest.raises(ValueError):
        render_prompt([{"role": "tool", "content": "x"}], None)


def test_to_trl_messages_folds_tools_into_system():
    s = _chat()
    s["tools"] = ["goto"]
    m = to_trl_messages(s)
    assert [x["role"] for x in m] == ["system", "user", "assistant"]
    assert m[0]["content"].startswith("SYS") and '"goto"' in m[0]["content"]
    assert "<|im_start|>" not in m[0]["content"]
