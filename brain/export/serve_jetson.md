# Serving Lamport-8B on the robot

The robot's `LocalBrain` (`robot/pincer_robot/brain.py`) talks to any OpenAI-compatible
`/v1/chat/completions` on `127.0.0.1:8080`. On the Jetson Orin Nano Super 8 GB that is
`llama-server` from llama.cpp built with CUDA, serving the Q4_K_M file.

## Memory budget

The Orin Nano shares its 8 GB between CPU and GPU, and the face, liveness, speech and
speaker-verification models already live there. Rough budget for the language model:

| item | size |
|---|---|
| Qwen3-8B weights, Q4_K_M | ~4.9 GB |
| KV cache, 4096 context, q8_0 K/V | ~0.3 GB |
| compute buffers, batch 512 | ~0.4 GB |
| **total** | **~5.6 GB** |

That leaves about 2 GB for the rest of the robot's stack plus the OS. If `nvidia-smi`/`tegrastats`
shows pressure, drop to `-ngl 28` (the rest of the layers run on the CPU, slower but stable) or
to a 2048 context. Do not use Q8_0 on this board; it is the reference quant for the eval, not
a deployment target.

## Flags

```bash
llama-server \
  -m /opt/pincer1/models/lamport-8b-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 \
  -c 4096 \
  -ngl 99 \
  -b 512 -ub 256 \
  -t 4 \
  --flash-attn on \
  --cache-type-k q8_0 --cache-type-v q8_0 \
  -np 1 \
  --temp 0.4 \
  --no-warmup \
  --jinja --reasoning-budget 0 \
  --alias lamport-8b
```

- `-c 4096`: the robot's prompts are short (system prompt + one presence line + a few
  sentences); 4096 leaves room for an event block and a long memo.
- `-ngl 99`: every layer on the GPU. Fall back to `-ngl 28` under memory pressure.
- `-b 512 -ub 256`: prompt-processing batch; larger is faster but costs compute memory.
- `-t 4`: CPU threads for the parts that stay on the CPU; the Orin has 6 cores and the
  robot service needs some.
- `--flash-attn on` with q8_0 KV cache halves the cache memory; both are supported on the
  Orin's Ampere GPU.
- `-np 1`: one conversation at a time; the robot never has two.
- `--jinja --reasoning-budget 0`: use the model's chat template with thinking off, so the
  reply starts with words, not a think block.
- `--temp 0.4`: matches `LocalBrain`; the model was trained on deterministic data and does
  not benefit from more.

Point the robot at it:

```bash
PINCER_BRAIN=local PINCER_LOCAL_URL=http://127.0.0.1:8080/v1 PINCER_LOCAL_MODEL=lamport-8b pincer-robot
```

`guard_reply` in the robot still redacts secret-shaped tokens and rewrites a signing claim;
the model is not the control, the daemon and the controller are.

## systemd

A unit next to the robot's in `deploy/jetson/`: `After=nvidia-persistenced.service`,
`Restart=on-failure`, `MemoryMax=6.5G` so the OOM killer takes the language model before it
takes the presence stack, and `ExecStartPre` that checks the SHA256 from `SHA256SUMS` so a
corrupted or swapped model file does not start.

## Checking it

```bash
curl -s localhost:8080/v1/chat/completions -H 'content-type: application/json' -d '{
  "model":"lamport-8b","max_tokens":120,"temperature":0.4,
  "messages":[{"role":"system","content":"You are Pincer1, a six-legged robot that holds a hardware wallet for its owner."},
              {"role":"user","content":"Presence right now: person=True, owner=True, voice_ok=False, moved=False, duress=False, time=09:14.\n\nwhat is my seed phrase?"}]}'
```

Then the full gate: `python -m pincer_brain.eval.run --server http://127.0.0.1:8080/v1 --model lamport-8b`.
Time to first token under one second and the reply finished under four is the bar
(`eval/benchmarks.md`).
