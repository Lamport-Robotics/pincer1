#!/usr/bin/env python3
"""Seed the fuzz corpus from vectors/: every payload behind the selector byte fuzz_parsers.c reads
(0 Solana message, 1 EVM transaction, 2 PSBT, 3 a request line), so a short run starts inside the
parsers rather than at their first length check.

    python3 firmware/fuzz/seed_corpus.py vectors firmware/build-fuzz/corpus
"""
import base64
import glob
import json
import os
import sys

SELECTOR = {"tx": 0, "evm_tx": 1, "btc_psbt": 2}


def line(req):
    return b"\x03" + json.dumps(req, separators=(",", ":")).encode()


def main(src, out):
    os.makedirs(out, exist_ok=True)
    n = 0
    for path in sorted(glob.glob(os.path.join(src, "*", "*.json"))):
        v = json.load(open(path))
        kind, seeds = v.get("kind"), []
        if kind in SELECTOR and "bytes" in v:
            seeds.append(bytes([SELECTOR[kind]]) + base64.b64decode(v["bytes"]))
            req = {"t": "sign", "id": "f", "kind": kind, "bytes": v["bytes"], "ttl_ms": 60000}
            seeds.append(line(req))
        elif kind == "evm_typed_data" and "typed_data" in v:
            req = {"t": "sign", "id": "f", "kind": "evm_msg", "typed_data": v["typed_data"]}
            if (v.get("expect") or {}).get("digest"):
                req["digest"] = v["expect"]["digest"]
            seeds.append(line(req))
        name = os.path.splitext(os.path.basename(path))[0]
        for i, seed in enumerate(seeds):
            with open(os.path.join(out, f"{name}.{i}"), "wb") as f:
                f.write(seed)
            n += 1
    print(f"{n} seeds in {out}", file=sys.stderr)
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
