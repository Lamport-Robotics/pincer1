"""Continued pretraining on the domain corpus (``corpus/out/cpt.jsonl``).

LoRA r=64 on every linear layer by default, or ``full_parameter: true`` in the config.
Documents are tokenized and packed into ``seq_len`` windows; the number of optimizer steps
comes from ``token_budget`` (1-5B tokens) divided by tokens per step.

    python -m pincer_brain.train.cpt --config train/configs/cpt_a100_80gb.yaml          # plan only
    python -m pincer_brain.train.cpt --config train/configs/cpt_h100.yaml --run        # trains
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

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


def _pack(ds, tokenizer, seq_len: int, text_field: str):
    """Tokenize, concatenate with EOS between documents, cut into seq_len blocks."""

    def tokenize(batch):
        ids = tokenizer(batch[text_field], add_special_tokens=False)["input_ids"]
        return {"input_ids": [x + [tokenizer.eos_token_id] for x in ids]}

    def group(batch):
        flat = [t for doc in batch["input_ids"] for t in doc]
        total = (len(flat) // seq_len) * seq_len
        chunks = [flat[i : i + seq_len] for i in range(0, total, seq_len)]
        return {"input_ids": chunks, "labels": [c[:] for c in chunks]}

    tokenized = ds.map(tokenize, batched=True, remove_columns=ds.column_names)
    return tokenized.map(group, batched=True)


def train(cfg, plan) -> None:
    from datasets import load_dataset
    from peft import get_peft_model
    from transformers import DataCollatorForLanguageModeling, Trainer, TrainingArguments

    model, tok = load_model_and_tokenizer(cfg)
    if cfg.lora:
        model = get_peft_model(model, lora_config(cfg))
        model.print_trainable_parameters()
    ds = load_dataset("json", data_files=cfg.data_path)["train"]
    ds = ds.shuffle(seed=cfg.seed)
    split = ds.train_test_split(test_size=cfg.eval_fraction, seed=cfg.seed)
    train_ds = _pack(split["train"], tok, cfg.seq_len, cfg.text_field)
    eval_ds = _pack(split["test"], tok, cfg.seq_len, cfg.text_field)
    args = TrainingArguments(
        output_dir=cfg.output_dir,
        per_device_train_batch_size=cfg.micro_batch,
        per_device_eval_batch_size=cfg.micro_batch,
        gradient_accumulation_steps=cfg.grad_accum,
        max_steps=plan.steps,
        learning_rate=cfg.lr,
        lr_scheduler_type="cosine",
        warmup_ratio=cfg.warmup_ratio,
        weight_decay=cfg.weight_decay,
        bf16=cfg.bf16,
        logging_steps=cfg.logging_steps,
        save_steps=cfg.save_steps,
        save_total_limit=3,
        eval_strategy="steps",
        eval_steps=cfg.save_steps,
        gradient_checkpointing=cfg.gradient_checkpointing,
        report_to=cfg.extras.get("report_to", "none"),
        seed=cfg.seed,
        dataloader_num_workers=4,
    )
    trainer = Trainer(
        model=model,
        args=args,
        train_dataset=train_ds,
        eval_dataset=eval_ds,
        data_collator=DataCollatorForLanguageModeling(tok, mlm=False),
    )
    trainer.train()
    trainer.save_model(cfg.output_dir)
    tok.save_pretrained(cfg.output_dir)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    common_args(ap, "cpt_a100_80gb.yaml")
    args = ap.parse_args(argv)
    cfg = resolve(args)
    rows, tokens = estimate_jsonl(cfg.data_path, cfg.text_field)
    plan = make_plan(cfg, tokens or None, rows or None)
    print_plan(cfg, plan)
    if not args.run or args.dry_run:
        print("dry run: pass --run to train", file=sys.stderr)
        return 0
    if not Path(cfg.data_path).exists():
        print(f"data not found: {cfg.data_path}", file=sys.stderr)
        return 2
    write_provenance(cfg, plan)
    train(cfg, plan)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
