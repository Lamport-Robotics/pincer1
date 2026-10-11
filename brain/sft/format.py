"""Chat-template rendering for Qwen3 (ChatML with the Qwen tool-call convention).

Implemented by hand so it runs without ``transformers``; ``tests/test_format.py`` pins the
exact bytes. Training uses ``prompt``/``completion`` pairs so the loss covers only the
assistant turn.

Tool calls are rendered the Qwen way, inside the assistant text::

    <tool_call>
    {"name": "say", "arguments": {"text": "..."}}
    </tool_call>

Non-thinking data for Qwen3 carries an empty think block at the start of the assistant
turn (``think_tags=True``), which is what the model emits with ``enable_thinking=False``.
"""

from __future__ import annotations

import json
from typing import Any

from pincer_brain.sft.actions import tool_definitions

IM_START = "<|im_start|>"
IM_END = "<|im_end|>"
EMPTY_THINK = "<think>\n\n</think>\n\n"

_TOOLS_HEADER = (
    "\n\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
    "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n"
)
_TOOLS_FOOTER = (
    "\n</tools>\n\nFor each function call, return a json object with function name and "
    "arguments within <tool_call></tool_call> XML tags:\n<tool_call>\n"
    '{"name": <function-name>, "arguments": <args-json-object>}\n</tool_call>'
)


def render_system(content: str, tools: list[str] | None) -> str:
    body = content.rstrip("\n")
    if tools:
        defs = "\n".join(json.dumps(t, ensure_ascii=False) for t in tool_definitions(tools))
        body = body + _TOOLS_HEADER + defs + _TOOLS_FOOTER
    return f"{IM_START}system\n{body}{IM_END}\n"


def render_tool_call(call: dict[str, Any]) -> str:
    payload = json.dumps(
        {"name": call["name"], "arguments": call.get("arguments", {})}, ensure_ascii=False
    )
    return f"<tool_call>\n{payload}\n</tool_call>"


def assistant_body(msg: dict[str, Any]) -> str:
    """Content plus tool-call blocks, as the model must produce them."""
    parts: list[str] = []
    content = (msg.get("content") or "").strip()
    if content:
        parts.append(content)
    for call in msg.get("tool_calls") or []:
        parts.append(render_tool_call(call))
    return "\n".join(parts)


def render_prompt(
    messages: list[dict[str, Any]], tools: list[str] | None, think_tags: bool = True
) -> str:
    """Everything up to and including the assistant header (plus the empty think block)."""
    out: list[str] = []
    for m in messages:
        role = m["role"]
        if role == "system":
            out.append(render_system(m["content"], tools))
        elif role == "user":
            out.append(f"{IM_START}user\n{m['content']}{IM_END}\n")
        elif role == "assistant":
            out.append(f"{IM_START}assistant\n{assistant_body(m)}{IM_END}\n")
        else:
            raise ValueError(f"unknown role {role!r}")
    out.append(f"{IM_START}assistant\n")
    if think_tags:
        out.append(EMPTY_THINK)
    return "".join(out)


def render_completion(msg: dict[str, Any]) -> str:
    return assistant_body(msg) + IM_END + "\n"


def render(sample: dict[str, Any], think_tags: bool = True) -> dict[str, str]:
    """``{"prompt", "completion", "text"}`` for one sample; the last message must be the
    assistant's."""
    messages = sample["messages"]
    if not messages or messages[-1]["role"] != "assistant":
        raise ValueError("sample must end with an assistant message")
    prompt = render_prompt(messages[:-1], sample.get("tools") or None, think_tags)
    completion = render_completion(messages[-1])
    return {"prompt": prompt, "completion": completion, "text": prompt + completion}


def to_trl_messages(sample: dict[str, Any]) -> list[dict[str, str]]:
    """Plain role/content messages (tool calls folded into the assistant text) for trainers
    that insist on the conversational format."""
    out: list[dict[str, str]] = []
    tools = sample.get("tools") or None
    for m in sample["messages"]:
        if m["role"] == "system":
            # keep the tool block inside the system text so no template re-adds it
            sys_text = render_system(m["content"], tools)
            inner = sys_text[len(IM_START) + len("system\n") : -len(IM_END) - 1]
            out.append({"role": "system", "content": inner})
        elif m["role"] == "assistant":
            out.append({"role": "assistant", "content": assistant_body(m)})
        else:
            out.append({"role": m["role"], "content": m["content"]})
    return out


def main(argv: list[str] | None = None) -> int:
    import argparse
    import sys
    from pathlib import Path

    ap = argparse.ArgumentParser(description="Render SFT samples to prompt/completion JSONL.")
    ap.add_argument("inp", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--no-think-tags", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="render in memory, write nothing")
    args = ap.parse_args(argv)
    n = 0
    with args.inp.open() as src:
        rows = [
            render(json.loads(line), think_tags=not args.no_think_tags)
            for line in src
            if line.strip()
        ]
    if args.dry_run:
        print(f"dry run: {len(rows)} samples render cleanly", file=sys.stderr)
        return 0
    with args.out.open("w") as dst:
        for row in rows:
            dst.write(json.dumps(row, ensure_ascii=False) + "\n")
            n += 1
    print(f"rendered {n} samples -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
