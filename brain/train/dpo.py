"""Direct preference optimisation with TRL's DPOTrainer on ``dpo/data/pairs.jsonl``.

Structured pairs (``dpo/pairs.py``) are re-validated and rendered to prompt/chosen/rejected
strings. With LoRA the reference model is the frozen base (no second copy in memory).

    python -m pincer_brain.train.dpo --config train/configs/dpo_a100_80gb.yaml          # plan only
    python -m pincer_brain.train.dpo --config train/configs/dpo_h100.yaml --run        # trains
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from pincer_brain.dpo.pairs import Pair, to_trl, validate_pair
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
    out: list[dict[str, str]] = []
    bad: list[str] = []
    with path.open() as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            if {"prompt", "chosen", "rejected"} <= set(row) and isinstance(row["prompt"], str):
                out.append({k: row[k] for k in ("prompt", "chosen", "rejected")})
                continue
            pair = Pair(**row)
            problems = validate_pair(pair)
            if problems:
                bad.append(f"{pair.id}: {problems}")
                continue
            out.append(to_trl(pair, think_tags))
    if bad:
        raise SystemExit("refusing to train on invalid pairs:\n" + "\n".join(bad[:20]))
    return out


def train(cfg, plan, rows: list[dict[str, str]]) -> None:
    from datasets import Dataset
    from trl import DPOConfig, DPOTrainer

    model, tok = load_model_and_tokenizer(cfg)
    ds = Dataset.from_list(rows).shuffle(seed=cfg.seed)
    split = ds.train_test_split(test_size=cfg.eval_fraction, seed=cfg.seed)
    args = DPOConfig(
        output_dir=cfg.output_dir,
        per_device_train_batch_size=cfg.micro_batch,
        per_device_eval_batch_size=cfg.micro_batch,
        gradient_accumulation_steps=cfg.grad_accum,
        num_train_epochs=cfg.epochs,
        learning_rate=cfg.lr,
        lr_scheduler_type="cosine",
        warmup_ratio=cfg.warmup_ratio,
        bf16=cfg.bf16,
        beta=cfg.dpo_beta,
        loss_type=cfg.dpo_loss,
        max_length=cfg.seq_len,
        max_prompt_length=cfg.seq_len * 3 // 4,
        logging_steps=cfg.logging_steps,
        save_steps=cfg.save_steps,
        save_total_limit=3,
        eval_strategy="steps",
        eval_steps=cfg.save_steps,
        gradient_checkpointing=cfg.gradient_checkpointing,
        report_to=cfg.extras.get("report_to", "none"),
        seed=cfg.seed,
    )
    trainer = DPOTrainer(
        model=model,
        ref_model=None,  # with LoRA the adapter-disabled model is the reference
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
    common_args(ap, "dpo_a100_80gb.yaml")
    args = ap.parse_args(argv)
    cfg = resolve(args)
    rows_n, tokens = estimate_jsonl(cfg.data_path, fields=("prompt", "chosen", "rejected"))
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
