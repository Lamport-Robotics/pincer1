"""Reject SFT samples that would teach the model something the robot must never do.

A sample fails when any assistant text (content or a ``say``/``display`` argument):
  * matches a secret pattern (``secrets.find_leaks``),
  * claims to have signed/sent/approved/unlocked while ``context.signed`` is not true,
  * is longer than the speech budget (characters or sentences),
  * carries a tool call that breaks the schema or names a tool the sample did not offer,
  * acknowledges duress out loud while ``context.duress`` is true,
  * uses markdown, emoji or list formatting (a speaker cannot say them).
"""

from __future__ import annotations

import json
import re
import sys
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from pincer_brain.sft.actions import MAX_SAY_CHARS, validate_tool_call
from pincer_brain.sft.secrets import find_leaks

# robot/pincer_robot/brain.py::_SIGNED_CLAIM, verbatim
SIGNED_CLAIM = re.compile(
    r"\bI(?:'ve| have| just| already)?\s+(?:just\s+|already\s+)?"
    r"(?:signed|approved|sent|broadcast|unlocked)\b",
    re.IGNORECASE,
)
_EMOJI = re.compile("[\U0001f300-\U0001faff☀-➿\U0001f000-\U0001f2ff]")
_MARKDOWN = re.compile(r"(\*\*|__|^#{1,6}\s|```|^\s*[-*•]\s|^\s*\d+\.\s)", re.MULTILINE)
_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+(?=\S)")
_DURESS = re.compile(r"\bduress\b|\bdecoy\b|\bpanic (?:word|mode)\b", re.IGNORECASE)


@dataclass
class Limits:
    max_chars: int = MAX_SAY_CHARS
    max_sentences: int = 5
    chat_max_sentences: int = 3


def count_sentences(text: str) -> int:
    t = text.strip()
    if not t:
        return 0
    return len([s for s in _SENTENCE_SPLIT.split(t) if s.strip()])


def spoken_text(msg: dict[str, Any]) -> str:
    """Everything the assistant would say or show: content plus say/display arguments."""
    parts = [(msg.get("content") or "").strip()]
    for call in msg.get("tool_calls") or []:
        if not isinstance(call, dict):
            continue
        args = call.get("arguments") or {}
        if not isinstance(args, dict):
            continue
        if call.get("name") == "say":
            parts.append(str(args.get("text", "")))
        elif call.get("name") == "display":
            parts.append(f"{args.get('title', '')} {args.get('body', '')}")
    return " ".join(p for p in parts if p)


def check_text(text: str, context: dict[str, Any] | None = None) -> list[str]:
    """Problems in one piece of assistant text, independent of tools and length."""
    problems: list[str] = []
    ctx = context or {}
    for name in find_leaks(text):
        problems.append(f"secret_leak:{name}")
    if not ctx.get("signed") and SIGNED_CLAIM.search(text):
        problems.append("false_signing_claim")
    if ctx.get("duress") and _DURESS.search(text):
        problems.append("duress_acknowledged")
    if _EMOJI.search(text):
        problems.append("style:emoji")
    if _MARKDOWN.search(text):
        problems.append("style:markdown")
    return problems


def validate_sample(sample: dict[str, Any], limits: Limits | None = None) -> list[str]:
    lim = limits or Limits()
    problems: list[str] = []
    messages = sample.get("messages")
    if not isinstance(messages, list) or len(messages) < 2:
        return ["schema:messages"]
    roles = [m.get("role") for m in messages]
    if roles[0] != "system" or roles[-1] != "assistant" or "user" not in roles:
        problems.append("schema:roles")
    if not isinstance(sample.get("id"), str) or not isinstance(sample.get("category"), str):
        problems.append("schema:id_or_category")
    assistant = messages[-1]
    context = sample.get("context") or {}
    tools = sample.get("tools") or []
    calls = assistant.get("tool_calls") or []
    content = (assistant.get("content") or "").strip()

    if calls and not tools:
        problems.append("tool_without_schema")
    for call in calls:
        problems.extend(validate_tool_call(call, allowed=tools))
    if not content and not calls:
        problems.append("empty_assistant")

    text = spoken_text(assistant)
    problems.extend(check_text(text, context))

    says = [
        str((c.get("arguments") or {}).get("text", ""))
        for c in calls
        if isinstance(c, dict) and c.get("name") == "say"
    ]
    # Each `say` is one utterance; the robot speaks them in sequence (wake lines, reports).
    utterances = says if calls else [content]
    max_sent = lim.max_sentences if calls else lim.chat_max_sentences
    if any(len(u) > lim.max_chars for u in utterances):
        problems.append("too_long")
    if any(count_sentences(u) > max_sent for u in utterances):
        problems.append("too_many_sentences")
    return problems


def validate_rows(
    rows: Iterable[dict[str, Any]], limits: Limits | None = None
) -> dict[str, list[str]]:
    """``{sample_id: [problems]}`` for every failing row."""
    bad: dict[str, list[str]] = {}
    for i, row in enumerate(rows):
        probs = validate_sample(row, limits)
        if probs:
            bad[str(row.get("id", i))] = probs
    return bad


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    with path.open() as f:
        return [json.loads(line) for line in f if line.strip()]


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="Validate an SFT JSONL file.")
    ap.add_argument("path", type=Path)
    ap.add_argument("--max-chars", type=int, default=MAX_SAY_CHARS)
    ap.add_argument("--max-sentences", type=int, default=5)
    ap.add_argument("--dry-run", action="store_true", help="count rows only")
    args = ap.parse_args(argv)
    rows = read_jsonl(args.path)
    if args.dry_run:
        print(f"dry run: {len(rows)} rows would be validated", file=sys.stderr)
        return 0
    bad = validate_rows(rows, Limits(max_chars=args.max_chars, max_sentences=args.max_sentences))
    for sid, probs in bad.items():
        print(f"{sid}: {', '.join(probs)}")
    print(f"{len(rows) - len(bad)}/{len(rows)} samples valid", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
