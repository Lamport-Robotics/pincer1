"""pincer-calibrate: measure each servo's zero and direction, write the calibration file.

The kinematics speak radians about a neutral the geometry defines: coxa along its mount, femur
horizontal, tibia vertical (interior knee angle 90°), arm straight up with the camera level. A servo
knows none of that, so for every joint we find the pulse (or tick) at which the real part sits in
that neutral and which way it turns for a positive angle. Two passes:

* ``jog``: one joint at a time, drive it with the keyboard until the part is at its neutral, press
  enter. A square against the plate and a plumb line are enough.
* ``signs``: each joint is nudged by about ten degrees and you answer whether it went the right way.

The result is ``~/.pincer1/servo_calibration.json`` (``body/pulse.py``), which the ``joint`` backend
loads at start. Run it over rosbridge (the first chassis) or on the serial bus (the v1 body).
"""

from __future__ import annotations

import argparse
import logging
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from .body.gaze import GazeConfig
from .body.geometry import LEG_NAMES, BodyGeometry
from .body.pulse import PULSES_PER_RAD, PulseCalibration, RosbridgeClient, RosbridgeServoBus

log = logging.getLogger("pincer.calibrate")

NEUTRAL_KNEE = math.pi / 2


@dataclass(frozen=True)
class Joint:
    servo_id: int
    name: str
    neutral_rad: float  # the kinematic angle of the part when it sits in the neutral pose
    neutral_text: str  # what to put the part at
    positive_text: str  # what a positive angle looks like


def joints(geometry: BodyGeometry, head: GazeConfig | None = None) -> list[Joint]:
    out: list[Joint] = []
    for leg, name in enumerate(LEG_NAMES):
        c, f, t = geometry.servo_ids(leg)
        out += [
            Joint(
                c,
                f"{name} coxa",
                0.0,
                "leg pointing straight out along its mount",
                "foot swings toward the front",
            ),
            Joint(f, f"{name} femur", 0.0, "femur horizontal", "knee lifts"),
            Joint(
                t,
                f"{name} tibia",
                NEUTRAL_KNEE,
                "tibia vertical, foot under the knee",
                "foot lifts (knee opens)",
            ),
        ]
    if head is not None:
        out += [
            Joint(head.pan_id, "arm yaw", 0.0, "arm pointing straight ahead", "head turns left"),
            Joint(head.shoulder_id, "arm shoulder", 0.0, "upper arm vertical", "arm tips backward"),
            Joint(
                head.elbow_id,
                "arm elbow",
                0.0,
                "forearm in line with the upper arm",
                "forearm tips backward",
            ),
            Joint(head.tilt_id, "arm wrist", 0.0, "camera level", "camera tips down"),
        ]
    return out


def solve_zero(pulse_at_neutral: float, neutral_rad: float, sign: int) -> int:
    """The pulse at which the kinematic angle is zero, from the pulse read at the neutral pose."""
    return int(round(pulse_at_neutral - sign * neutral_rad * PULSES_PER_RAD))


JOG_KEYS = {"a": -10, "d": 10, "s": -2, "w": 2}


def jog_step(key: str) -> int | None:
    """Pulses to move for one keypress, None for anything that is not a jog key."""
    return JOG_KEYS.get(key.strip().lower()[:1]) if key.strip() else None


class Session:
    def __init__(self, bus: RosbridgeServoBus, cal: PulseCalibration, *, ask=input, say=print):
        self.bus, self.cal, self.ask, self.say = bus, cal, ask, say
        self.neutral_pulse: dict[int, float] = {}

    def _set(self, servo_id: int, pulse: float, seconds: float = 0.4) -> None:
        self.bus.client.publish(
            "/servo_controller",
            {
                "duration": seconds,
                "position_unit": "pulse",
                "position": [{"id": servo_id, "position": float(pulse)}],
            },
        )
        time.sleep(seconds + 0.1)

    def jog(self, joint: Joint) -> float:
        pulse = self.bus.pulse(joint.servo_id)
        self.say(
            f"\n{joint.name} (servo {joint.servo_id}) is at {pulse:.0f}. "
            f"Put it at: {joint.neutral_text}."
        )
        self.say("  a/d: -10/+10   s/w: -2/+2   enter: done   q: skip this joint")
        while True:
            key = self.ask("> ")
            if key.strip().lower() == "q":
                return float("nan")
            if key.strip() == "":
                self.neutral_pulse[joint.servo_id] = pulse
                return pulse
            step = jog_step(key)
            if step is None:
                continue
            pulse = max(0.0, min(1000.0, pulse + step))
            self._set(joint.servo_id, pulse)
            self.say(f"  {pulse:.0f}")

    def sign(self, joint: Joint) -> int:
        p0 = self.neutral_pulse.get(joint.servo_id, self.bus.pulse(joint.servo_id))
        nudge = 10 * math.pi / 180 * PULSES_PER_RAD  # ten degrees
        self._set(joint.servo_id, p0 + nudge, 0.6)
        answer = self.ask(f"{joint.name}: did the {joint.positive_text}? [y/n] ")
        self._set(joint.servo_id, p0, 0.6)
        return 1 if answer.strip().lower().startswith("y") else -1

    def run(self, todo: list[Joint], *, do_jog: bool, do_signs: bool) -> PulseCalibration:
        for j in todo:
            if do_jog:
                p = self.jog(j)
                if math.isnan(p):
                    continue
            else:
                p = self.bus.pulse(j.servo_id)
                self.neutral_pulse[j.servo_id] = p
            s = self.sign(j) if do_signs else self.cal.sign.get(j.servo_id, 1)
            self.cal.sign[j.servo_id] = s
            self.cal.zero[j.servo_id] = solve_zero(p, j.neutral_rad, s)
            self.say(f"  {j.name}: neutral {p:.0f}, sign {s:+d}, zero {self.cal.zero[j.servo_id]}")
        return self.cal


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pincer-calibrate", description=__doc__.split("\n\n")[0])
    ap.add_argument("--rosbridge", default="ws://127.0.0.1:9090", help="the chassis' rosbridge")
    ap.add_argument("--out", default="~/.pincer1/servo_calibration.json")
    ap.add_argument("--legs", action="store_true", help="only the eighteen leg servos")
    ap.add_argument("--arm", action="store_true", help="only the four arm servos")
    ap.add_argument("--no-jog", action="store_true", help="take the current pose as the neutral")
    ap.add_argument("--no-signs", action="store_true", help="keep the signs already in the file")
    ap.add_argument("--show", action="store_true", help="print the live pulses and exit")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    out = Path(a.out).expanduser()
    cal = PulseCalibration.load(out) if out.exists() else PulseCalibration()
    client = RosbridgeClient(a.rosbridge)
    bus = RosbridgeServoBus(client, cal)
    if not bus.wait_for_state(5.0):
        print("no servo state from the chassis; is its controller running?", file=sys.stderr)
        return 1
    if a.show:
        for sid, p in sorted(bus.pulses().items()):
            print(f"servo {sid:2d}: {p:.0f}")
        return 0
    g = BodyGeometry()
    todo = joints(g, None if a.legs else GazeConfig())
    if a.arm:
        todo = [j for j in todo if j.servo_id >= 19]
    session = Session(bus, cal)
    print(
        "calibration: every servo stays powered; keep hands clear of the legs while a joint moves."
    )
    cal = session.run(todo, do_jog=not a.no_jog, do_signs=not a.no_signs)
    cal.note = f"measured {time.strftime('%Y-%m-%d')} over rosbridge"
    cal.save(out)
    print(f"wrote {out}")
    client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
