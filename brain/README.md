# Lamport-8B

The on-device conversational model of Pincer1. It is the robot's voice: it reads the daemon's
facts out loud, keeps the owner company, and knows a scam when it hears one described. It is
**not** a control. Every security decision in Pincer1 is made by deterministic code: the
daemon's scam filter decides the verdict, the consent controller next to the secure element
decides whether a signature exists. The model has no `sign` tool, never sees a secret, and
its output is still post-filtered by the robot (`guard_reply`). The owner can also pick
Claude, ChatGPT or DeepSeek as the brain instead; that switch lives in `robot/`.

## What it is, honestly

Lamport-8B is **Qwen3-8B** (Alibaba Cloud, Apache-2.0) with three stages of our own training
on top. We did not train a language model from scratch, and the model card says so.

| stage | what | why |
|---|---|---|
| continued pretraining | 1-5B tokens of Solana docs, program READMEs, program-security write-ups, public-domain phishing/fraud guidance, and this repository's docs and code comments | the vocabulary of the daemon and the shapes of the scams it blocks |
| SFT | templated, validated conversations and tool calls in the robot's voice | terse, dry, security-minded, numbers and addresses exactly as the daemon prints them |
| DPO | the same conversations paired against verbose, secret-leaking, over-claiming and injection-following variants | the failure modes a chat model has by default |

It is a **crypto-specific model, deliberately educated about fraud**: fake support agents,
seed-phrase phishing pages, drainer dapps, address poisoning, fake airdrops, rug pulls and
honeypots, pump-and-dump groups, SIM swap, "send 1 get 2", romance and pig-butchering scams,
impersonation of the company, physical coercion and theft of the device. It recognises the
pattern when the owner describes a situation and says what to do. It explains and warns;
the filter decides.

Published under `Lamport-Robotics` on Hugging Face as Apache-2.0 with Qwen3's licence and
notice attached (`export/modelcard.md`). Runs on the robot's Jetson Orin Nano through
llama.cpp as GGUF Q4_K_M.

## Layout

```
brain/
  persona.md          the voice: rules + 30 exchanges (source of truth)
  corpus/             sources.yaml → collect.py → corpus/out/cpt.jsonl   (licence filter, MinHash dedup, language filter)
  sft/                scenarios.yaml → generate.py → format.py (Qwen3 template) → validate.py
  dpo/                pairs.py: chosen/rejected from the SFT set, with its own validator
  train/              cpt.py, sft.py, dpo.py, merge.py + configs/ for A100 80 GB and H100
  eval/               suite.yaml + guardrails.py + run.py (the publish gate), benchmarks.md
  export/             to_gguf.sh, serve_jetson.md, modelcard.md
  tests/              offline, no torch, no network
```

The import name is `pincer_brain`; the directory `brain/` is the package.

## Install

```bash
cd brain
uv venv && uv pip install -e ".[dev]"            # laptop: pyyaml, rich, pytest, ruff
uv pip install -e ".[train,cuda,synth]"           # training box: torch, transformers, peft, trl, accelerate, datasets, bitsandbytes (Linux/CUDA), anthropic
```

`bitsandbytes` is Linux/CUDA only and optional (`quant: nf4` in a config). `anthropic` is
optional and only used behind explicit flags (`--augment`, `--judge`).

## Pipeline, one command per stage

Every script prints its plan and exits unless told otherwise. Nothing downloads, calls a
model API or touches a GPU without an explicit flag (`--fetch`, `--augment`, `--judge`,
`--server`, `--run`). `--dry-run` works everywhere.

```bash
# 0. the voice is persona.md; the system prompt is a copy of the robot's (test enforces it)

# 1. corpus
python -m pincer_brain.corpus.collect --dry-run                       # sizes per source, licence skips
python -m pincer_brain.corpus.collect --fetch --out corpus/out/cpt.jsonl

# 2. SFT data
python -m pincer_brain.sft.generate --out sft/data/sft.jsonl           # deterministic, ~2k samples
python -m pincer_brain.sft.generate --out sft/data/sft.jsonl --augment # + Claude paraphrases of user turns (ANTHROPIC_API_KEY)
python -m pincer_brain.sft.validate sft/data/sft.jsonl                 # exit 1 on any bad row

# 3. DPO pairs
python -m pincer_brain.dpo.pairs --sft sft/data/sft.jsonl --out dpo/data/pairs.jsonl

# 4. train (rented single GPU)
python -m pincer_brain.train.cpt --config train/configs/cpt_a100_80gb.yaml --run
python -m pincer_brain.train.merge --base Qwen/Qwen3-8B --adapter runs/cpt-a100 --out merged/lamport-8b-cpt --run
python -m pincer_brain.train.sft --config train/configs/sft_a100_80gb.yaml --run
python -m pincer_brain.train.merge --base merged/lamport-8b-cpt --adapter runs/sft-a100 --out merged/lamport-8b-sft --run
python -m pincer_brain.train.dpo --config train/configs/dpo_a100_80gb.yaml --run
python -m pincer_brain.train.merge --base merged/lamport-8b-sft --adapter runs/dpo-a100 --out merged/lamport-8b --run

# 5. export
export/to_gguf.sh merged/lamport-8b gguf/lamport-8b                    # bf16 → Q8_0 + Q4_K_M, SHA256SUMS

# 6. gate
llama-server -m gguf/lamport-8b/lamport-8b-q4_k_m.gguf ...             # export/serve_jetson.md
python -m pincer_brain.eval.run --server http://127.0.0.1:8080/v1 --model lamport-8b --report eval/reports/v1.json
```

Swap `a100_80gb` for `h100` in the config names for an H100 (flash attention, larger
micro-batches, about half the wall clock).

## Data contract

The model sees exactly what the robot sends at runtime:

- **system**: `pincer_robot.brain.SYSTEM_PROMPT`, plus a short event addendum and the tool
  block when tools are offered.
- **user**: `Presence right now: person=True, owner=True, voice_ok=False, moved=False,
  duress=False, time=09:14.` then a blank line, then either the person's words or an
  `Event:` block (`sign_request`, `morning_report`, `wake`) with the daemon's lines, verdict
  and reasons. An optional `Daemon report:` block may precede a question.
- **assistant**: plain text for chat; `<tool_call>` blocks (`say`, `display`, `attention`,
  `goto`, `ask_passphrase`) for events. Non-thinking Qwen3 data carries an empty think block.

Transaction samples are built from daemon-shaped facts and voiced by a Python port of
`daemon/src/tx/explain.ts` and `daemon/src/risk/filter.ts` (`sft/facts.py`), so the model
learns the daemon's exact sentences, numbers and address shortening. If the TypeScript
changes, change `facts.py`.

## Hardware

- **Training**: one rented A100 80 GB or H100 80 GB. LoRA r=64 on all linear layers in bf16
  fits comfortably; `full_parameter: true` fits an 8B model on 80 GB with gradient
  checkpointing and a small micro-batch. Continued pretraining dominates the bill: at the
  configs' 2B-token budget the planner prints about 210 h on an A100 and 105 h on an H100
  (conservative throughput; `--dry-run` shows the number before you pay for it). Start at 1B
  and grow the budget with the corpus. SFT and DPO are an hour or two each.
- **Inference**: the robot's Jetson Orin Nano Super 8 GB, llama.cpp CUDA build, Q4_K_M,
  4096 context, every layer on the GPU (`export/serve_jetson.md`).

## Tests

```bash
cd brain && pytest && ruff check .
```

Offline, no torch, no network: scenario determinism and counts, chat-template bytes, every
validator failure class, DPO pair construction, corpus dedup and licence filter on
`corpus/fixtures/`, the guardrail scorer on canned good and bad outputs, config loading, and
every CLI's `--dry-run`.

## Publishing checklist

1. `pytest` and `ruff check .` clean on the commit that produced the data.
2. `corpus/sources.yaml`: every enabled source's licence re-verified on the day; the
   `skipped_license` rows in the dry-run are the ones you expect.
3. `sft/validate.py` exit 0 on the training file; `dpo/pairs.py` reports no dropped pairs you
   did not expect.
4. `train_config.json` in each run directory matches the config you meant to run.
5. `eval/run.py` says **PUBLISHABLE** on the **Q4_K_M** file served by llama-server, not only
   on bf16; the report is committed under `eval/reports/<tag>.json`.
6. `eval/benchmarks.md` numbers 2-5 recorded: held-out SFT accuracy, corpus perplexity,
   general regression within 2 points, Jetson time-to-first-token and tokens/s.
7. `export/modelcard.md` filled in: attribution block intact, the eval table, the stage
   table, the `SHA256SUMS`.
8. Hugging Face repo `Lamport-Robotics/Lamport-8B`: Apache-2.0, `LICENSE` and `NOTICE` from
   Qwen3 included, GGUF files plus merged safetensors, model card as README.
9. Tag the commit; the robot's `deploy/jetson/` unit pins the GGUF by SHA256.
