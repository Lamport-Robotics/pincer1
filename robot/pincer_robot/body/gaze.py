"""Gaze: the camera head follows whoever the presence layer is looking at.

The head rides the four-axis arm. ``Gaze`` is the pure part: it takes where a face sits in the image
(normalised, -1..1 on both axes) and turns it into a pan / tilt pair in radians, closing a fraction
of the error per update, rate limited, inside the arm's limits. When the face goes away it holds the
last bearing for a moment, then drifts back to centre and scans slowly until someone shows up.

``GazeHead`` is the wiring: the biggest face from the recogniser in, one sync-write to the arm servos
out, on the same bus the legs use. The other two arm joints are parked in the upright pose so the
camera sits as high as the arm allows; nothing here touches the legs.

Frames follow ``sim/build.py``: the arm's pitch joints turn about +y, so a positive wrist angle tips
the camera down. ``tilt`` here is camera elevation (positive up) and is negated on the way out.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

from .servo import Bus


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


@dataclass(frozen=True)
class ArmPose:
    """The two arm joints gaze never moves, radians. Zero is the arm pointing straight up."""

    shoulder: float = 0.0
    elbow: float = 0.0


@dataclass
class GazeConfig:
    pan_id: int = 19
    shoulder_id: int = 20
    elbow_id: int = 21
    tilt_id: int = 22
    hfov_deg: float = 62.0
    vfov_deg: float = 48.0
    pan_limits: tuple[float, float] = (-math.radians(70), math.radians(70))
    tilt_limits: tuple[float, float] = (-math.radians(35), math.radians(45))  # elevation
    gain: float = 0.7  # fraction of the measured error closed per update
    deadband: float = 0.04  # normalised image units; smaller errors are left alone
    max_rate_rad_s: float = 1.5
    hold_s: float = 2.0  # keep the last bearing this long after the face is lost
    scan_after_s: float = 6.0  # then centre and start scanning
    scan_period_s: float = 10.0
    scan_amplitude: float = math.radians(35)
    scan_tilt: float = math.radians(5)  # a little above level, where standing people are
    rest: ArmPose = field(default_factory=ArmPose)


@dataclass(frozen=True)
class GazeTarget:
    pan: float
    tilt: float
    mode: str  # tracking | hold | scan


class Gaze:
    def __init__(self, config: GazeConfig | None = None, *, now: float | None = None):
        self.cfg = config or GazeConfig()
        self.pan = 0.0
        self.tilt = 0.0
        self.mode = "scan"
        t = time.monotonic() if now is None else now
        self._last_update = t
        self._last_seen = -math.inf
        self._scan_start: float | None = None

    @property
    def target(self) -> GazeTarget:
        return GazeTarget(self.pan, self.tilt, self.mode)

    def update(self, face: tuple[float, float] | None, now: float) -> GazeTarget:
        """``face`` is the face centre as (x, y) in -1..1 (right and down positive), or None."""
        dt = max(0.0, now - self._last_update)
        self._last_update = now
        c = self.cfg
        if face is not None:
            self._last_seen = now
            self._scan_start = None
            self.mode = "tracking"
            x, y = face
            want_pan, want_tilt = self.pan, self.tilt
            if abs(x) > c.deadband:
                want_pan = self.pan - c.gain * x * math.radians(c.hfov_deg) / 2.0
            if abs(y) > c.deadband:
                want_tilt = self.tilt - c.gain * y * math.radians(c.vfov_deg) / 2.0
        elif now - self._last_seen < c.hold_s:
            self.mode = "hold"
            want_pan, want_tilt = self.pan, self.tilt
        else:
            self.mode = "scan"
            if self._scan_start is None:
                self._scan_start = now
            t = now - self._scan_start
            # settle time first, then a slow sweep about the centre
            sweep = 0.0 if t < max(0.0, c.scan_after_s - c.hold_s) else t - (c.scan_after_s - c.hold_s)
            want_pan = c.scan_amplitude * math.sin(2 * math.pi * sweep / c.scan_period_s)
            want_tilt = c.scan_tilt
        step = c.max_rate_rad_s * dt
        self.pan = clamp(self.pan + clamp(want_pan - self.pan, -step, step), *c.pan_limits)
        self.tilt = clamp(self.tilt + clamp(want_tilt - self.tilt, -step, step), *c.tilt_limits)
        return self.target

    # --- servo targets -------------------------------------------------------------------
    def head_targets(self) -> dict[int, float]:
        """Pan and tilt as joint angles for the bus; tilt flips sign (see module docstring)."""
        return {self.cfg.pan_id: self.pan, self.cfg.tilt_id: -self.tilt}

    def arm_targets(self) -> dict[int, float]:
        """The whole arm: shoulder and elbow in the rest pose plus the current gaze."""
        out = {self.cfg.shoulder_id: self.cfg.rest.shoulder, self.cfg.elbow_id: self.cfg.rest.elbow}
        out.update(self.head_targets())
        return out


def largest_face(observations: list) -> tuple[float, float] | None:
    """Centre of the biggest face box as (x, y) in -1..1, or None. Works on anything with
    ``bbox``, ``x_norm`` and ``y_norm`` (``presence.face.FaceObservation``)."""
    best, area = None, -1.0
    for o in observations:
        x1, y1, x2, y2 = o.bbox
        a = max(0.0, x2 - x1) * max(0.0, y2 - y1)
        if a > area:
            best, area = o, a
    if best is None:
        return None
    return (float(best.x_norm), float(best.y_norm))


class GazeHead:
    """Drives the arm servos from face observations. One sync-write per update, skipped while the
    target has not moved, so the bus stays free for the gait between frames."""

    def __init__(self, bus: Bus, gaze: Gaze | None = None, *, move_time_ms: int = 120):
        self.bus = bus
        self.gaze = gaze or Gaze()
        self.move_time_ms = move_time_ms
        self._sent: dict[int, float] = {}

    def raise_head(self, seconds: float = 1.5) -> None:
        """Bring the arm to the upright rest pose with the camera level; call once at start."""
        self.bus.torque(sorted(self.gaze.arm_targets()), True)
        self.gaze.pan, self.gaze.tilt = 0.0, 0.0
        targets = self.gaze.arm_targets()
        self.bus.move(targets, time_ms=int(seconds * 1000))
        self._sent = dict(targets)

    def observe(self, observations: list, ts: float) -> GazeTarget:
        target = self.gaze.update(largest_face(observations), ts)
        wanted = self.gaze.head_targets()
        if any(k not in self._sent or abs(wanted[k] - self._sent[k]) > 1e-3 for k in wanted):
            self.bus.move(wanted, time_ms=self.move_time_ms)
            self._sent.update(wanted)
        return target
