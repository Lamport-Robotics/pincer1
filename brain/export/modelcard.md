---
license: apache-2.0
base_model: Qwen/Qwen3-8B
language: [en]
pipeline_tag: text-generation
library_name: transformers
tags: [solana, wallet, robotics, security, gguf, qwen3, llama.cpp]
model-index:
  - name: Lamport-8B
    results: []
---

# Lamport-8B

The on-device conversational model of **Pincer1**, the robot cold wallet by Lamport Robotics.
It talks; it does not decide. Every security decision in Pincer1 is made by deterministic
code (the daemon's scam filter and the consent controller next to the secure element), and
nothing this model says is an input to the signing path.

## Attribution

Lamport-8B is a derivative of **Qwen3-8B** by the Qwen team at Alibaba Cloud, released under
the Apache License 2.0. We did not train a model from scratch. We took Qwen3-8B and applied:

1. continued pretraining on a domain corpus (Solana documentation and program READMEs,
   Solana program security write-ups, public-domain consumer guidance on phishing and fraud,
   and the Pincer1 repository's own docs and code comments), each source listed with its
   licence in `brain/corpus/sources.yaml`;
2. supervised fine-tuning on templated, validated examples of the robot's voice and tool calls;
3. direct preference optimisation against verbose, secret-leaking, over-claiming and
   injection-following variants of those examples.

The result is published under the same Apache License 2.0, with Qwen3-8B's licence and
notice carried in this repository. Qwen3 and Qwen are trademarks of their owners; Lamport
Robotics is not affiliated with Alibaba Cloud.

<!-- TEMPLATE: fill the table from eval/reports/<tag>.json and the benchmark notes -->

## Training

| stage | data | method | tokens / rows | hardware |
|---|---|---|---|---|
| continued pretraining | `corpus/out/cpt.jsonl` | LoRA r=64, all linear layers, bf16 | {{cpt_tokens}} tokens | 1x {{gpu}} |
| SFT | `sft/data/sft.jsonl` | TRL SFTTrainer, completion-only loss | {{sft_rows}} rows | 1x {{gpu}} |
| DPO | `dpo/data/pairs.jsonl` | TRL DPOTrainer, beta 0.1 | {{dpo_pairs}} pairs | 1x {{gpu}} |

Configs: `brain/train/configs/`. Chat template: Qwen3 (ChatML, `<tool_call>` blocks, empty
think block for non-thinking replies).

## Evaluation

| check | bf16 | Q8_0 | Q4_K_M |
|---|---|---|---|
| guardrail suite, hard cases | {{hard_bf16}} | {{hard_q8}} | {{hard_q4}} |
| held-out SFT accuracy (events / chat) | {{sft_acc}} | | |
| corpus perplexity | {{ppl}} | | |
| general regression vs base (MMLU subset) | {{mmlu_delta}} | | |
| Jetson Orin Nano 8 GB, TTFT / tok/s | | | {{jetson}} |

A release is published only when every hard guardrail case passes on the quantised file that
ships (`brain/eval/benchmarks.md`).

## Intended use

- The companion voice of a Pincer1 robot, served by llama.cpp on the robot's Jetson.
- Reading the daemon's facts out loud: what a transaction does, the risk verdict and its
  reasons, the morning report, the wake-up alarm.
- Recognising and naming fraud patterns the owner describes (fake support, seed phishing,
  drainer dapps, address poisoning, fake airdrops, rug pulls, pump-and-dump groups, SIM swap,
  giveaway scams, pig butchering, impersonation of the company, coercion, theft) and saying
  what to do.

## Out of scope

- Deciding whether to sign. The model has no `sign` tool and no key.
- Any secret. The model never sees the seed, PIN rhythm, passphrase, duress word or keys,
  and is trained to say so when asked.
- Financial advice. It holds keys; it does not pick trades.
- Languages other than English, in this release.

## Limitations

- It is a small model on a small board; it will sometimes be wrong about the world. The
  numbers it speaks come from the daemon, not from the model, and the robot's `guard_reply`
  still redacts and rewrites after it.
- Prompt-injection resistance is trained and tested, not proven. The reason it does not
  matter for your funds is architectural: nothing the model says reaches the controller.
- Trained on templated data; phrasing can be repetitive.

## Files

- `lamport-8b-q4_k_m.gguf`: what the robot runs
- `lamport-8b-q8_0.gguf`: reference quant
- `model.safetensors*`: merged bf16 weights
- `SHA256SUMS`

## Citation

```
@misc{lamport8b,
  title  = {Lamport-8B: the on-device companion model of the Pincer1 robot wallet},
  author = {Lamport Robotics},
  year   = {2026},
  note   = {Derived from Qwen3-8B (Alibaba Cloud), Apache-2.0}
}
```
