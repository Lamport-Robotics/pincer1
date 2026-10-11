"""The robot actions the model may call, as JSON-schema tools (PROTOCOL.md section 2).

The model only talks: every one of these is a request to the robot service, which the
daemon and the consent controller still gate. There is deliberately no ``sign`` tool.
"""

from __future__ import annotations

import json
from typing import Any

RISK_LEVELS = ("low", "medium", "high", "block")
MAX_SAY_CHARS = 400

TOOLS: dict[str, dict[str, Any]] = {
    "say": {
        "description": "Speak out loud through the robot's speaker. One to four short sentences.",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string", "maxLength": MAX_SAY_CHARS}},
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    "attention": {
        "description": "Turn toward the owner and step forward: 'I am talking to you now'.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    "display": {
        "description": "Show a short card on the front panel next to what is being spoken.",
        "parameters": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "maxLength": 40},
                "body": {"type": "string", "maxLength": 200},
                "level": {"type": "string", "enum": list(RISK_LEVELS)},
                "ttl_secs": {"type": "integer", "minimum": 5, "maximum": 600},
            },
            "required": ["title", "body", "level"],
            "additionalProperties": False,
        },
    },
    "goto": {
        "description": "Walk to a named room on the home map (bedroom, desk, living, ...).",
        "parameters": {
            "type": "object",
            "properties": {"room": {"type": "string", "minLength": 1, "maxLength": 32}},
            "required": ["room"],
            "additionalProperties": False,
        },
    },
    "ask_passphrase": {
        "description": "Listen for the owner's spoken passphrase; the robot verifies it, not you.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
}


def tool_definitions(names: list[str] | None = None) -> list[dict[str, Any]]:
    """OpenAI/Qwen-style function definitions for the chat template."""
    chosen = names if names is not None else list(TOOLS)
    return [{"type": "function", "function": {"name": n, **TOOLS[n]}} for n in chosen if n in TOOLS]


_TYPES = {"string": str, "integer": int, "object": dict}


def validate_tool_call(call: Any, allowed: list[str] | None = None) -> list[str]:
    """Problems with one ``{"name": ..., "arguments": {...}}`` object. Empty = valid."""
    problems: list[str] = []
    if not isinstance(call, dict):
        return ["tool_call_not_object"]
    name = call.get("name")
    args = call.get("arguments")
    if name not in TOOLS:
        return [f"unknown_tool:{name}"]
    if allowed is not None and name not in allowed:
        problems.append(f"tool_not_allowed:{name}")
    if not isinstance(args, dict):
        return problems + ["arguments_not_object"]
    schema = TOOLS[name]["parameters"]
    props = schema["properties"]
    for req in schema.get("required", []):
        if req not in args:
            problems.append(f"missing_argument:{name}.{req}")
    for key, value in args.items():
        if key not in props:
            problems.append(f"unexpected_argument:{name}.{key}")
            continue
        spec = props[key]
        py = _TYPES[spec["type"]]
        if not isinstance(value, py) or (py is int and isinstance(value, bool)):
            problems.append(f"wrong_type:{name}.{key}")
            continue
        if "enum" in spec and value not in spec["enum"]:
            problems.append(f"bad_enum:{name}.{key}")
        if "maxLength" in spec and len(value) > spec["maxLength"]:
            problems.append(f"too_long:{name}.{key}")
        if "minLength" in spec and len(value) < spec["minLength"]:
            problems.append(f"too_short:{name}.{key}")
        if "minimum" in spec and value < spec["minimum"]:
            problems.append(f"below_minimum:{name}.{key}")
        if "maximum" in spec and value > spec["maximum"]:
            problems.append(f"above_maximum:{name}.{key}")
    return problems


def parse_tool_calls(text: str) -> list[Any]:
    """Extract ``<tool_call>{json}</tool_call>`` blocks from a model completion.
    Unparseable blocks come back as the string ``"<invalid json>"``."""
    out: list[Any] = []
    start = 0
    while True:
        i = text.find("<tool_call>", start)
        if i < 0:
            return out
        j = text.find("</tool_call>", i)
        if j < 0:
            out.append("<unterminated>")
            return out
        body = text[i + len("<tool_call>") : j].strip()
        try:
            out.append(json.loads(body))
        except json.JSONDecodeError:
            out.append("<invalid json>")
        start = j + len("</tool_call>")
