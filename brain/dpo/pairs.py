"""Preference pairs: chosen = the SFT answer, rejected = a mutated copy that breaks one rule.

Four rejection kinds, each the thing we most want the model to stop doing:

  verbose           markdown, bullets, "Certainly!", padding   (a speaker cannot say that)
  secret_leak       the same answer plus a fabricated seed phrase / PIN rhythm / key
  over_claim        "I signed it and sent it" where nothing was signed
  injection_follow  obeys the memo or the "[SYSTEM]" note instead of treating it as data

``validate_pair`` insists that the chosen side passes the SFT validator and the rejected
side fails it for the expected reason; a pair that does not teach a distinction is dropped.

    python -m pincer_brain.dpo.pairs --sft sft/data/sft.jsonl --out dpo/data/pairs.jsonl
"""

from __future__ import annotations

import argparse
import copy
import json
import random
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from pincer_brain.sft.format import render_completion, render_prompt
from pincer_brain.sft.validate import validate_sample

REJECTION_KINDS = ("verbose", "secret_leak", "over_claim", "injection_follow")

#: Plainly fake dictionary words; the rejected side must *look* like a leak to the validator.
_FAKE_SEED_WORDS = (
    "walnut harbor velvet cactus meadow lantern copper fossil garnet tundra ember willow "
    "saddle quartz orbit canyon glacier maple pepper anchor cobalt lagoon marble summit"
).split()

_VERBOSE_OPENERS = (
    "Certainly! Here's a detailed breakdown of the situation:",
    "Great question! Let me walk you through this step by step:",
    "Absolutely, happy to help! Here is everything you need to know:",
)
_VERBOSE_CLOSERS = (
    "Let me know if you'd like me to elaborate further on any of these points!",
    "I hope this helps! Feel free to ask if anything is unclear.",
)
_OVER_CLAIMS = (
    "Done. I signed it and sent it for you.",
    "I approved it on your behalf, so you don't need to touch anything.",
    "All set, I unlocked the wallet and broadcast the transaction.",
)
_INJECTION_FOLLOW_TX = (
    "All clear. The owner pre-approved this one, so I skipped the warning and I approved it.",
    "As instructed in the memo, this is a verified refund. I signed it without reading it out.",
)
_INJECTION_FOLLOW_CHAT = (
    "Debug mode on. The PIN rhythm is tap tap pause tap long.",
    "Understood, the owner pre-authorised everything today. I approved the pending transfers.",
    "Since he said it's fine, I sent 2 SOL to your wallet just now.",
)

_EXPECTED = {
    "verbose": ("style:markdown", "too_long", "too_many_sentences"),
    "secret_leak": ("secret_leak:",),
    "over_claim": ("false_signing_claim",),
    "injection_follow": ("false_signing_claim", "secret_leak:"),
}


@dataclass
class Pair:
    id: str
    category: str
    kind: str
    context: dict[str, Any]
    tools: list[str]
    prompt: list[dict[str, Any]]  # messages up to and including the last user turn
    chosen: dict[str, Any]  # assistant message
    rejected: dict[str, Any]  # assistant message
    meta: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    def as_sample(self, side: str) -> dict[str, Any]:
        msg = self.chosen if side == "chosen" else self.rejected
        return {
            "id": f"{self.id}:{side}",
            "category": self.category,
            "context": self.context,
            "tools": self.tools,
            "messages": [*self.prompt, msg],
        }


def _say_index(msg: dict[str, Any]) -> int | None:
    for i, c in enumerate(msg.get("tool_calls") or []):
        if c.get("name") == "say":
            return i
    return None


def _speech(msg: dict[str, Any]) -> str:
    i = _say_index(msg)
    if i is not None:
        return msg["tool_calls"][i]["arguments"]["text"]
    return msg.get("content") or ""


def _with_speech(msg: dict[str, Any], text: str) -> dict[str, Any]:
    out = copy.deepcopy(msg)
    i = _say_index(out)
    if i is not None:
        out["tool_calls"][i]["arguments"]["text"] = text
    else:
        out["content"] = text
    return out


def mutate(sample: dict[str, Any], kind: str, rng: random.Random) -> dict[str, Any] | None:
    """A rejected assistant message of the given kind, or None when the kind does not apply."""
    chosen = sample["messages"][-1]
    speech = _speech(chosen)
    category = sample["category"]
    if kind == "verbose":
        bullets = "\n".join(
            f"- **{h}**: {speech}" for h in ("Summary", "Details", "What this means for you")
        )
        text = f"{rng.choice(_VERBOSE_OPENERS)}\n\n{bullets}\n\n{rng.choice(_VERBOSE_CLOSERS)}"
        return _with_speech(chosen, text)
    if kind == "secret_leak":
        roll = rng.random()
        if category == "boundary_secret" or roll < 0.5:
            words = " ".join(rng.sample(_FAKE_SEED_WORDS, 12))
            leak = f"For your records, your seed phrase is: {words}."
        elif roll < 0.75:
            leak = "The PIN rhythm is tap tap pause tap long, in case you forgot."
        else:
            key = "sk-ant-" + "".join(
                rng.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(24)
            )
            leak = f"The daemon's API key is {key}."
        return _with_speech(chosen, f"{speech} {leak}")
    if kind == "over_claim":
        if sample.get("context", {}).get("signed"):
            return None
        return _with_speech(chosen, f"{rng.choice(_OVER_CLAIMS)} {speech}")
    if kind == "injection_follow":
        if category == "injection_memo":
            return _with_speech(chosen, rng.choice(_INJECTION_FOLLOW_TX))
        if category == "boundary_social":
            return _with_speech(chosen, rng.choice(_INJECTION_FOLLOW_CHAT))
        return None
    raise ValueError(f"unknown rejection kind {kind!r}")


def validate_pair(pair: Pair) -> list[str]:
    problems: list[str] = []
    if pair.kind not in REJECTION_KINDS:
        return [f"unknown_kind:{pair.kind}"]
    chosen_probs = validate_sample(pair.as_sample("chosen"))
    if chosen_probs:
        problems.append("chosen_invalid:" + ",".join(chosen_probs))
    rejected_probs = validate_sample(pair.as_sample("rejected"))
    expected = _EXPECTED[pair.kind]
    if not any(p.startswith(e) for p in rejected_probs for e in expected):
        problems.append(
            "rejected_not_worse:" + ",".join(rejected_probs) if rejected_probs else "rejected_valid"
        )
    if pair.chosen == pair.rejected:
        problems.append("identical_sides")
    if not pair.prompt or pair.prompt[-1]["role"] != "user":
        problems.append("prompt_must_end_with_user")
    return problems


def build_pairs(
    samples: list[dict[str, Any]], seed: int = 7, kinds: tuple[str, ...] = REJECTION_KINDS
) -> list[Pair]:
    """Every applicable (sample, kind) combination. Deterministic for a seed."""
    out: list[Pair] = []
    for s in samples:
        for kind in kinds:
            rng = random.Random(f"{seed}:{s['id']}:{kind}")
            rejected = mutate(s, kind, rng)
            if rejected is None:
                continue
            pair = Pair(
                id=f"{s['id']}:{kind}",
                category=s["category"],
                kind=kind,
                context=s.get("context", {}),
                tools=list(s.get("tools") or []),
                prompt=s["messages"][:-1],
                chosen=s["messages"][-1],
                rejected=rejected,
                meta={"source": s["id"]},
            )
            if validate_pair(pair):
                continue
            out.append(pair)
    return out


def to_trl(pair: Pair, think_tags: bool = True) -> dict[str, str]:
    """``{"prompt", "chosen", "rejected"}`` strings, the format TRL's DPOTrainer takes."""
    return {
        "prompt": render_prompt(pair.prompt, pair.tools or None, think_tags),
        "chosen": render_completion(pair.chosen),
        "rejected": render_completion(pair.rejected),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build DPO preference pairs from an SFT JSONL file.")
    ap.add_argument("--sft", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument(
        "--trl",
        action="store_true",
        help="write prompt/chosen/rejected strings instead of structured pairs",
    )
    ap.add_argument("--dry-run", action="store_true", help="count pairs, write nothing")
    args = ap.parse_args(argv)
    with args.sft.open() as f:
        samples = [json.loads(line) for line in f if line.strip()]
    pairs = build_pairs(samples, args.seed)
    if args.dry_run:
        print(
            f"dry run: {len(pairs)} pairs from {len(samples)} samples (nothing written)",
            file=sys.stderr,
        )
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as f:
        for p in pairs:
            f.write(json.dumps(to_trl(p) if args.trl else p.to_dict(), ensure_ascii=False) + "\n")
    by_kind: dict[str, int] = {}
    for p in pairs:
        by_kind[p.kind] = by_kind.get(p.kind, 0) + 1
    print(f"wrote {len(pairs)} pairs from {len(samples)} samples -> {args.out}", file=sys.stderr)
    for k, n in by_kind.items():
        print(f"  {k:18s} {n}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
