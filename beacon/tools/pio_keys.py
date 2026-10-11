# PlatformIO pre-script: bake the provisioning file into the firmware as -D macros.
#
# Reads $BEACON_ENV or beacon/beacon.env (written by tools/provision.py). Without a valid file
# the build gets -DBEACON_UNPROVISIONED=1: it compiles, but the beacon refuses to arm and the base
# refuses to decode. tools/keyflags.py prints the same flags for a manual PLATFORMIO_BUILD_FLAGS.
import os
import sys

Import("env")  # noqa: F821  (PlatformIO/SCons injects it)

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))   # no __file__ under SCons
import keyflags  # noqa: E402

defines = []
for name, value, is_str in keyflags.macros():
    if value is None:
        defines.append(name)
    elif is_str:
        defines.append((name, env.StringifyMacro(value)))
    else:
        defines.append((name, value))
env.Append(CPPDEFINES=defines)
