#!/usr/bin/env python3
"""Build flags for the native test env: link the host's mbedTLS when it is installed.

Prints `-DFINDMY_HOST_MBEDTLS -I<include> -L<lib> -lmbedcrypto` when mbedtls/ecp.h and
libmbedcrypto are found (Homebrew, /usr/local, /usr), otherwise nothing. With mbedTLS the Find My
test also checks the P-224 public keys against the Python helper; without it that single test is
skipped and `pio test` says so in the summary (the KDF, scalar derivation and payload tests run
either way).
"""
import os
import subprocess

CANDIDATES = ["/opt/homebrew/opt/mbedtls", "/usr/local/opt/mbedtls", "/usr/local", "/usr"]
try:
    prefix = subprocess.run(["brew", "--prefix", "mbedtls"], capture_output=True, text=True, timeout=10).stdout.strip()
    if prefix:
        CANDIDATES.insert(0, prefix)
except Exception:  # brew absent
    pass

for p in CANDIDATES:
    inc = os.path.join(p, "include", "mbedtls", "ecp.h")
    libdir = os.path.join(p, "lib")
    if os.path.exists(inc) and any(
        os.path.exists(os.path.join(libdir, f"libmbedcrypto{ext}")) for ext in (".a", ".dylib", ".so")
    ):
        print(f"-DFINDMY_HOST_MBEDTLS=1 -I{os.path.join(p, 'include')} -L{libdir} -lmbedcrypto")
        break
else:
    print("")
