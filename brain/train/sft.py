"""Supervised fine-tuning with TRL's SFTTrainer on ``sft/data/sft.jsonl``.

The structured samples are rendered to ``prompt``/``completion`` with the Qwen3 template
(``sft/format.py``) so the loss covers only the assistant turn, tool calls included. Every
row is re-validated before training; a failing row aborts the run.

    python -m pincer_brain.train.sft --config train/configs/sft_a100_80gb.yaml          # plan only
    python -m pincer_brain.train.sft --config train/configs/sft_h100.yaml --run        # trains
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from pincer_brain.sft.format import render
from pincer_brain.sft.validate import validate_sample
from pincer_brain.train.common import (
    common_args,
    estimate_jsonl,
    load_model_and_tokenizer,
    lora_config,
    make_plan,
    print_plan,
    resolve,
    write_provenance,
)


def prepare_rows(path: Path, think_tags: bool) -> list[dict[str, str]]:
    """Structured samples → prompt/completion; already-rendered rows pass through."""
    out: list[dict[str, str]] = []
    bad: list[str] = []
    with path.open() as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            if "prompt" in row and "completion" in row:
                out.append({"prompt": row["prompt"], "completion": row["completion"]})
                continue
            problems = validate_sample(row)
            if problems:
                bad.append(f"{row.get('id')}: {problems}")
                continue
            r = render(row, think_tags=think_tags)
            out.append({"prompt": r["prompt"], "completion": r["completion"]})
    if bad:
        raise SystemExit("refusing to train on invalid samples:\n" + "\n".join(bad[:20]))
    return out


def train(cfg, plan, rows: list[dict[str, str]]) -> None:
    from datasets import Dataset
    from trl import SFTConfig, SFTTrainer

    model, tok = load_model_and_tokenizer(cfg)
    ds = Dataset.from_list(rows).shuffle(seed=cfg.seed)
    split = ds.train_test_split(test_size=cfg.eval_fraction, seed=cfg.seed)
    args = SFTConfig(
        output_dir=cfg.output_dir,
        per_device_train_batch_size=cfg.micro_batch,
        per_device_eval_batch_size=cfg.micro_batch,
        gradient_accumulation_steps=cfg.grad_accum,
        num_train_epochs=cfg.epochs,
        learning_rate=cfg.lr,
        lr_scheduler_type="cosine",
        warmup_ratio=cfg.warmup_ratio,
        weight_decay=cfg.weight_decay,
        bf16=cfg.bf16,
        max_length=cfg.seq_len,
        packing=False,  # prompt/completion rows: completion-only loss, no packing
        completion_only_loss=True,
        logging_steps=cfg.logging_steps,
        save_steps=cfg.save_steps,
        save_total_limit=3,
        eval_strategy="steps",
        eval_steps=cfg.save_steps,
        gradient_checkpointing=cfg.gradient_checkpointing,
        report_to=cfg.extras.get("report_to", "none"),
        seed=cfg.seed,
    )
    trainer = SFTTrainer(
        model=model,
        args=args,
        train_dataset=split["train"],
        eval_dataset=split["test"],
        processing_class=tok,
        peft_config=lora_config(cfg) if cfg.lora else None,
    )
    trainer.train()
    trainer.save_model(cfg.output_dir)
    tok.save_pretrained(cfg.output_dir)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    common_args(ap, "sft_a100_80gb.yaml")
    args = ap.parse_args(argv)
    cfg = resolve(args)
    rows_n, tokens = estimate_jsonl(cfg.data_path, fields=("messages",))
    plan = make_plan(cfg, tokens or None, rows_n or None)
    print_plan(cfg, plan)
    if not args.run or args.dry_run:
        print("dry run: pass --run to train", file=sys.stderr)
        return 0
    path = Path(cfg.data_path)
    if not path.exists():
        print(f"data not found: {path}", file=sys.stderr)
        return 2
    rows = prepare_rows(path, cfg.think_tags)
    write_provenance(cfg, plan, {"rows": len(rows)})
    train(cfg, plan, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
