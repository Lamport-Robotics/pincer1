#!/usr/bin/env python3
"""Find My key list for the Pincer1 beacon.

Recreates, from the 32-byte master seed, exactly the rotating P-224 keys the beacon advertises
(src/findmy.h documents the derivation; this file mirrors it byte for byte, in pure Python with no
third-party modules) and writes them in the formats the fetchers take:

  --format openhaystack   one accessory with `privateKey` + `additionalKeys` (base64 private keys),
                          the JSON that OpenHaystack exports and that macless-haystack imports
  --format jsonl          one line per key: index, private key, advertised key (x), hashed adv key
  --format csv            same, as CSV

The beacon advances the key index every 15 minutes while it advertises (alarm) and keeps the index
in flash; a heartbeat or alarm packet tells the base the current index. Generate a comfortable
window (the default 2000 keys is 20 days of continuous alarm) and feed it to the fetcher; the
fetcher asks Apple for all of them at once.

  python3 tools/findmy_keys.py --env beacon.env --count 2000 --format openhaystack > findmy_keys.json

Pass --check to compare the pure-Python curve arithmetic against the `cryptography` package when it
is installed (it is not required).
"""
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import json
import os
import sys

# secp224r1 (NIST P-224)
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF000000000000000000000001
A = P - 3
B = 0xB4050A850C04B3ABF54132565044B0B7D7BFD8BA270B39432355FFB4
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFF16A2E0B8F03E13DD29455C5C2A3D
GX = 0xB70E0CBD6BB4BF7F321390B94A03C1D356C21122343280D6115C1D21
GY = 0xBD376388B5F723FB4C22DFE6CD4375A05A07476444D5819985007E34

ROTATE_SEC = 900


def x963_kdf(z: bytes, info: bytes, length: int) -> bytes:
    """ANSI X9.63 KDF with SHA-256: SHA256(Z || counter_be32 || info) for counter = 1, 2, ..."""
    out = b""
    counter = 1
    while len(out) < length:
        out += hashlib.sha256(z + counter.to_bytes(4, "big") + info).digest()
        counter += 1
    return out[:length]


def _scalar_in_range(b: bytes) -> int:
    return int.from_bytes(b, "big") % (N - 1) + 1


def master_from_seed(seed: bytes) -> tuple[int, bytes]:
    """(d0, SK_0) from the 32-byte seed. This part is ours (OpenHaystack draws them at random)."""
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    d0 = _scalar_in_range(x963_kdf(seed, b"pincer1-findmy-d0", 32))
    sk0 = x963_kdf(seed, b"pincer1-findmy-sk0", 32)
    return d0, sk0


def next_sk(sk: bytes) -> bytes:
    return x963_kdf(sk, b"update", 32)


def private_key_at(d0: int, sk: bytes) -> int:
    """d_i = (d0 * u_i + v_i) mod n with (u_i, v_i) = KDF(SK_i, "diversify", 72)."""
    uv = x963_kdf(sk, b"diversify", 72)
    u = _scalar_in_range(uv[:36])
    v = _scalar_in_range(uv[36:])
    return (d0 * u + v) % N


# --- P-224 point arithmetic, affine coordinates, nothing clever ----------------------------------

def _inv(x: int) -> int:
    return pow(x, -1, P)


def _add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        lam = (3 * x1 * x1 + A) * _inv(2 * y1) % P
    else:
        lam = (y2 - y1) * _inv(x2 - x1) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return x3, y3


def scalar_mult(k: int, point=(GX, GY)):
    result = None
    addend = point
    while k:
        if k & 1:
            result = _add(result, addend)
        addend = _add(addend, addend)
        k >>= 1
    return result


def public_x(d: int) -> bytes:
    x, _ = scalar_mult(d)
    return x.to_bytes(28, "big")


# --- key list -------------------------------------------------------------------------------------

def key_list(seed: bytes, count: int, start: int = 0):
    d0, sk = master_from_seed(seed)
    for _ in range(start):
        sk = next_sk(sk)
    for i in range(start, start + count):
        d = private_key_at(d0, sk)
        pub = public_x(d)
        yield {
            "index": i,
            "private_key": d.to_bytes(28, "big"),
            "adv_key": pub,
            "hashed_adv_key": hashlib.sha256(pub).digest(),
        }
        sk = next_sk(sk)


def read_env(path: str) -> dict[str, str]:
    env: dict[str, str] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def check_against_cryptography(seed: bytes, n: int = 3) -> None:
    try:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.kdf.x963kdf import X963KDF
    except ImportError:
        print("check: the cryptography package is not installed, skipped", file=sys.stderr)
        return
    for info in (b"update", b"diversify", b"pincer1-findmy-d0"):
        ref = X963KDF(algorithm=hashes.SHA256(), length=72, sharedinfo=info).derive(seed)
        assert ref == x963_kdf(seed, info, 72), "X9.63 KDF mismatch"
    for k in key_list(seed, n):
        d = int.from_bytes(k["private_key"], "big")
        pub = ec.derive_private_key(d, ec.SECP224R1()).public_key().public_numbers()
        assert pub.x.to_bytes(28, "big") == k["adv_key"], "P-224 mismatch at index %d" % k["index"]
    print("check: KDF and P-224 agree with the cryptography package for %d keys" % n, file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--env", help="beacon.env from tools/provision.py (reads BEACON_FINDMY_SEED)")
    src.add_argument("--seed", help="64 hex characters")
    ap.add_argument("--count", type=int, default=2000, help="number of keys (default 2000 = 20 days of alarm)")
    ap.add_argument("--start", type=int, default=0, help="first key index")
    ap.add_argument("--format", choices=["openhaystack", "jsonl", "csv"], default="openhaystack")
    ap.add_argument("--name", default="Pincer1 beacon", help="accessory name in the openhaystack format")
    ap.add_argument("--check", action="store_true", help="cross-check against the cryptography package if installed")
    args = ap.parse_args()

    if args.env:
        seed_hex = read_env(args.env).get("BEACON_FINDMY_SEED", "")
    else:
        seed_hex = args.seed
    try:
        seed = bytes.fromhex(seed_hex)
    except ValueError:
        seed = b""
    if len(seed) != 32:
        print("error: need a 32-byte seed (64 hex chars)", file=sys.stderr)
        return 2

    if args.check:
        check_against_cryptography(seed)

    keys = list(key_list(seed, args.count, args.start))
    b64 = lambda b: base64.b64encode(b).decode()  # noqa: E731

    if args.format == "openhaystack":
        acc = {
            "id": int.from_bytes(hashlib.sha256(seed).digest()[:4], "big") & 0x7FFFFFFF,
            "colorComponents": [0.85, 0.35, 0.1, 1],
            "name": args.name,
            "privateKey": b64(keys[0]["private_key"]),
            "icon": "",
            "isDeployed": True,
            "isActive": True,
            "additionalKeys": [b64(k["private_key"]) for k in keys[1:]],
        }
        json.dump([acc], sys.stdout, indent=1)
        sys.stdout.write("\n")
    elif args.format == "jsonl":
        for k in keys:
            sys.stdout.write(json.dumps({
                "index": k["index"],
                "private_key_b64": b64(k["private_key"]),
                "adv_key_hex": k["adv_key"].hex(),
                "hashed_adv_key_b64": b64(k["hashed_adv_key"]),
            }) + "\n")
    else:
        w = csv.writer(sys.stdout)
        w.writerow(["index", "private_key_b64", "adv_key_hex", "hashed_adv_key_b64"])
        for k in keys:
            w.writerow([k["index"], b64(k["private_key"]), k["adv_key"].hex(), b64(k["hashed_adv_key"])])
    return 0


if __name__ == "__main__":
    sys.exit(main())
