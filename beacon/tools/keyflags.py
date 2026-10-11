#!/usr/bin/env python3
"""Turn beacon.env into -D build flags.

Used by tools/pio_keys.py (the PlatformIO pre-script that bakes the keys into a build) and
runnable on its own to print the flags for a manual PLATFORMIO_BUILD_FLAGS.

Reads $BEACON_ENV, or beacon/beacon.env. Without a valid file it yields -DBEACON_UNPROVISIONED=1
so the firmware still builds (for compile checks) but refuses to arm. Diagnostics go to stderr.
"""
from __future__ import annotations

import os
import sys

REGIONS = ("EU868", "US915", "AS923")


def env_path() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    return os.environ.get("BEACON_ENV") or os.path.join(os.path.dirname(here), "beacon.env")


def read_env(path: str) -> dict[str, str]:
    env: dict[str, str] = {}
    if not os.path.exists(path):
        return env
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def _is_hex(s: str, nbytes: int) -> bool:
    if len(s) != 2 * nbytes:
        return False
    try:
        bytes.fromhex(s)
        return True
    except ValueError:
        return False


def macros() -> list[tuple[str, str | None, bool]]:
    """[(name, value or None, value_is_a_string_literal)]"""
    path = env_path()
    env = read_env(path)
    dev_id = env.get("BEACON_DEVICE_ID", "")
    key = env.get("BEACON_LORA_KEY", "")
    seed = env.get("BEACON_FINDMY_SEED", "")
    region = env.get("BEACON_REGION", "EU868").upper()
    hb_pos = env.get("BEACON_HEARTBEAT_POSITION", "1").strip().lower() in ("1", "true", "yes")

    out: list[tuple[str, str | None, bool]] = []
    if _is_hex(dev_id, 8) and _is_hex(key, 32) and _is_hex(seed, 32):
        out += [
            ("BEACON_DEVICE_ID_HEX", dev_id, True),
            ("BEACON_LORA_KEY_HEX", key, True),
            ("BEACON_FINDMY_SEED_HEX", seed, True),
        ]
    else:
        print(f"keyflags: no valid provisioning in {path}; building UNPROVISIONED (run tools/provision.py)", file=sys.stderr)
        out.append(("BEACON_UNPROVISIONED", "1", False))
    if region not in REGIONS:
        print(f"keyflags: unknown region {region}, using EU868", file=sys.stderr)
        region = "EU868"
    out.append((f"BEACON_REGION_{region}", "1", False))
    out.append(("BEACON_HEARTBEAT_POSITION", "1" if hb_pos else "0", False))
    return out


def compute() -> str:
    parts = []
    for name, value, is_str in macros():
        if value is None:
            parts.append(f"-D{name}")
        elif is_str:
            parts.append(f'-D{name}=\\"{value}\\"')
        else:
            parts.append(f"-D{name}={value}")
    return " ".join(parts)


if __name__ == "__main__":
    print(compute())
