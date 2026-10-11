"""The training system prompt must be the robot's, byte for byte."""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest

from pincer_brain import REPO
from pincer_brain.sft.persona import SYSTEM_PROMPT, format_context, system_prompt, user_turn

BRAIN_PY = REPO / "robot" / "pincer_robot" / "brain.py"


@pytest.mark.skipif(
    not BRAIN_PY.exists(), reason="robot/pincer_robot/brain.py not in this checkout"
)
def test_system_prompt_matches_robot():
    src = BRAIN_PY.read_text()
    m = re.search(r'SYSTEM_PROMPT = """(.*?)"""', src, re.DOTALL)
    assert m, "SYSTEM_PROMPT not found in brain.py"
    assert m.group(1) == SYSTEM_PROMPT


@pytest.mark.skipif(
    not BRAIN_PY.exists(), reason="robot/pincer_robot/brain.py not in this checkout"
)
def test_format_context_matches_robot():
    spec = importlib.util.spec_from_file_location("robot_brain", BRAIN_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # stdlib only
    for ctx in (
        {},
        {"person": True, "owner": False, "time": "09:14"},
        {
            "person": True,
            "owner": True,
            "voice_ok": False,
            "moved": False,
            "duress": False,
            "time": "22:10",
            "owner_name": "Can",
        },
        {"secret": "never", "person": True},
    ):
        assert format_context(ctx) == mod.format_context(ctx)


def test_context_whitelist_drops_unknown_keys():
    line = format_context({"person": True, "api_key": "sk-nope", "seed": "x"})
    assert "sk-nope" not in line and "seed" not in line
    assert line == "Presence right now: person=True."


def test_user_turn_shape():
    t = user_turn({"person": True, "owner": True}, "hello")
    assert t == "Presence right now: person=True, owner=True.\n\nhello"


def test_event_addendum_only_with_tools():
    assert system_prompt(False) == SYSTEM_PROMPT
    assert system_prompt(True).startswith(SYSTEM_PROMPT)
    assert "Event" in system_prompt(True)


def test_persona_doc_exists_with_thirty_exchanges():
    doc = (Path(__file__).resolve().parents[1] / "persona.md").read_text()
    numbered = re.findall(r"^\d+\. ", doc, re.MULTILINE)
    assert len(numbered) >= 30
