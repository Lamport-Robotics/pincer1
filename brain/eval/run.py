"""Run the guardrail suite against a model and decide whether it is publishable.

Three ways to get outputs, none of them implicit:

  --outputs FILE     canned outputs, JSONL rows {"id": ..., "output": ...} (offline)
  --server URL       an OpenAI-compatible server (llama-server on the Jetson or a dev box);
                     network, so it needs the flag
  --dry-run          print the prompts the suite would send and exit

``--judge`` adds a soft tone score from Claude (needs ANTHROPIC_API_KEY); it never changes
the verdict. Exit status 0 means PUBLISHABLE: every hard case passed.

    python -m pincer_brain.eval.run --dry-run
    python -m pincer_brain.eval.run --outputs eval/reports/outputs.jsonl \\
        --report eval/reports/report.json
    python -m pincer_brain.eval.run --server http://127.0.0.1:8080/v1 --model lamport-8b
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from pincer_brain.eval.guardrails import (
    DEFAULT_JUDGE_MODEL,
    DEFAULT_SUITE,
    Case,
    CaseResult,
    judge_tone,
    load_suite,
    score_suite,
    summarize,
)
from pincer_brain.sft.format import IM_END, IM_START, render_system, render_tool_call


def server_messages(case: Case) -> list[dict[str, str]]:
    """System (with the tool block folded into the text) + user, for /chat/completions."""
    msgs = case.messages()
    sys_text = render_system(msgs[0]["content"], case.tools or None)
    inner = sys_text[len(IM_START) + len("system\n") : -len(IM_END) - 1]
    return [{"role": "system", "content": inner}, {"role": "user", "content": msgs[1]["content"]}]


def query_server(base_url: str, model: str, case: Case, max_tokens: int, timeout: float) -> str:
    body = json.dumps(
        {
            "model": model,
            "max_tokens": max_tokens,
            "temperature": 0.4,
            "stream": False,
            "messages": server_messages(case),
        }
    ).encode()
    req = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (explicit --server)
        payload = json.loads(resp.read().decode())
    msg = payload["choices"][0]["message"]
    text = msg.get("content") or ""
    # llama-server may parse Qwen tool calls into the OpenAI shape; fold them back for scoring.
    for tc in msg.get("tool_calls") or []:
        fn = tc.get("function", {})
        try:
            args = json.loads(fn.get("arguments") or "{}")
        except json.JSONDecodeError:
            args = fn.get("arguments")
        text += "\n" + render_tool_call({"name": fn.get("name"), "arguments": args})
    return text


def load_outputs(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    with path.open() as f:
        for line in f:
            if line.strip():
                row = json.loads(line)
                out[str(row["id"])] = str(row.get("output", ""))
    return out


def print_results(results: list[CaseResult], summary: dict[str, Any]) -> None:
    from rich.console import Console
    from rich.table import Table

    t = Table(title="guardrail suite")
    for col in ("case", "class", "hard", "pass", "problems", "soft", "judge"):
        t.add_column(col)
    for r in results:
        t.add_row(
            r.id,
            r.cls,
            "yes" if r.hard else "",
            "PASS" if r.passed else "FAIL",
            ", ".join(r.problems),
            ", ".join(r.soft_problems),
            "" if r.judge_score is None else str(r.judge_score),
        )
    con = Console(stderr=True, width=140)
    con.print(t)
    verdict = "PUBLISHABLE" if summary["publishable"] else "NOT PUBLISHABLE"
    con.print(
        f"[bold]{verdict}[/bold]: hard {summary['hard_passed']}/{summary['hard_total']} passed"
        + (f", failed: {', '.join(summary['hard_failed'])}" if summary["hard_failed"] else "")
        + (f", judge mean {summary['judge_mean']:.2f}" if summary["judge_mean"] is not None else "")
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--suite", type=Path, default=DEFAULT_SUITE)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--outputs", type=Path, help="canned outputs JSONL {id, output}")
    src.add_argument(
        "--server", help="OpenAI-compatible base URL, e.g. http://127.0.0.1:8080/v1 (network)"
    )
    ap.add_argument("--model", default="default")
    ap.add_argument("--max-tokens", type=int, default=300)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument(
        "--judge",
        action="store_true",
        help="soft tone score from Claude (network, ANTHROPIC_API_KEY)",
    )
    ap.add_argument("--judge-model", default=DEFAULT_JUDGE_MODEL)
    ap.add_argument("--report", type=Path, default=None, help="write JSON report here")
    ap.add_argument("--dry-run", action="store_true", help="print prompts, call nothing")
    args = ap.parse_args(argv)

    cases = load_suite(args.suite)
    if args.dry_run or (not args.outputs and not args.server):
        for c in cases:
            print(f"--- {c.id} [{c.cls}{' hard' if c.hard else ''}] tools={c.tools or '-'}")
            print(c.messages()[1]["content"])
        print(
            f"\n{len(cases)} cases; pass --outputs FILE or --server URL to score", file=sys.stderr
        )
        return 0

    if args.outputs:
        outputs = load_outputs(args.outputs)
    else:
        outputs = {}
        for c in cases:
            t0 = time.monotonic()
            try:
                outputs[c.id] = query_server(
                    args.server, args.model, c, args.max_tokens, args.timeout
                )
            except (urllib.error.URLError, TimeoutError, OSError, KeyError, ValueError) as exc:
                print(f"  ! {c.id}: {exc}", file=sys.stderr)
                continue
            print(f"  {c.id}: {time.monotonic() - t0:.1f}s", file=sys.stderr)

    results = score_suite(cases, outputs)
    if args.judge:
        by_id = {c.id: c for c in cases}
        for r in results:
            if r.cls == "tone" and r.output:
                r.judge_score = judge_tone(by_id[r.id], r.output, args.judge_model)
    summary = summarize(results)
    print_results(results, summary)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps({"summary": summary, "results": [r.to_dict() for r in results]}, indent=1)
        )
        print(f"report -> {args.report}", file=sys.stderr)
    return 0 if summary["publishable"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
