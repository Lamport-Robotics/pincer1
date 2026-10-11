"""Merge a LoRA adapter into its base and save bf16 safetensors (+ tokenizer, provenance).

    python -m pincer_brain.train.merge --base Qwen/Qwen3-8B --adapter runs/cpt-a100 \\
        --out merged/lamport-8b-cpt
    python -m pincer_brain.train.merge --base merged/lamport-8b-cpt --adapter runs/sft \\
        --out merged/lamport-8b-sft --run

Plan only without ``--run``. A full-parameter run has no adapter: skip this step.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path


def merge(base: str, adapter: Path, out: Path) -> None:
    import torch
    from peft import PeftModel
    from transformers import AutoModelForCausalLM, AutoTokenizer

    model = AutoModelForCausalLM.from_pretrained(base, torch_dtype=torch.bfloat16)
    model = PeftModel.from_pretrained(model, str(adapter))
    model = model.merge_and_unload()
    out.mkdir(parents=True, exist_ok=True)
    model.save_pretrained(out, safe_serialization=True, max_shard_size="4GB")
    tok = AutoTokenizer.from_pretrained(
        str(adapter) if (adapter / "tokenizer_config.json").exists() else base
    )
    tok.save_pretrained(out)
    (out / "merge.json").write_text(
        json.dumps(
            {"base": base, "adapter": str(adapter), "at": int(time.time()), "dtype": "bfloat16"},
            indent=1,
        )
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--base", required=True, help="HF id or local path of the base the adapter was trained on"
    )
    ap.add_argument("--adapter", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--run",
        action="store_true",
        help="actually load and merge (needs the weights locally or network)",
    )
    args = ap.parse_args(argv)
    print(
        f"merge plan: base={args.base} adapter={args.adapter} -> {args.out} (bf16 safetensors)",
        file=sys.stderr,
    )
    if not args.run or args.dry_run:
        print("dry run: pass --run to merge", file=sys.stderr)
        return 0
    if not (args.adapter / "adapter_config.json").exists():
        print(f"no adapter_config.json in {args.adapter}", file=sys.stderr)
        return 2
    merge(args.base, args.adapter, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
