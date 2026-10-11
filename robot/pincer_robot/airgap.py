"""Air-gap relay: cut the robot's radio unless the owner is here (or the daemon asked).

A small relay on one Jetson 40-pin GPIO line switches the 5 V of the USB Wi-Fi / radio
dongle. ``AirGap`` decides, ``Relay`` drives the pin:

* ``off``    - relay always on (radio always powered). Daemon overrides are ignored.
* ``strict`` - radio on only while the owner is present, plus ``grace_s`` (60 s) after
  the last sighting. A daemon ``network on`` request is *ignored* in strict mode (that is
  the point of strict); a ``network off`` request is honoured for its ``for_secs``.
* ``night``  - like strict, but a daemon-requested window (``network on`` with
  ``for_secs``) also powers the radio - the agent's trading window while the owner sleeps.

Overrides come from ``{"t":"network","on":bool,"reason":str,"for_secs":int}`` and expire
on their own; ``evaluate(now)`` recomputes the desired state and only touches the relay on
a change. The published ``network`` field is the last state actually applied.

``JetsonGpioRelay`` imports ``Jetson.GPIO`` lazily (JetPack ships it; ``pip install
Jetson.GPIO`` otherwise; the service user needs the ``gpio`` group). BOARD numbering,
default pin 7 (GPIO09 / AUDIO_MCLK on the Orin Nano carrier). ``active_high`` matches the
relay module: most opto-isolated boards are active LOW - set ``PINCER_AIRGAP_ACTIVE_HIGH=0``
for those. Wiring and the fail-safe choice (should the radio be on or off when the Jetson is
down?) are hardware decisions documented in docs/HARDWARE.md, not made here.
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from typing import Literal, Protocol

log = logging.getLogger("pincer.airgap")

AirgapMode = Literal["off", "strict", "night"]
MODES: tuple[str, ...] = ("off", "strict", "night")


class Relay(Protocol):
    def set(self, on: bool) -> None: ...

    def close(self) -> None: ...


class MockRelay:
    def __init__(self) -> None:
        self.state: bool | None = None
        self.changes: list[bool] = []

    def set(self, on: bool) -> None:
        self.state = on
        self.changes.append(on)

    def close(self) -> None:
        pass


class JetsonGpioRelay:
    """Drives one BOARD pin through Jetson.GPIO; imported only when constructed."""

    def __init__(self, pin: int = 7, active_high: bool = True) -> None:
        import Jetson.GPIO as GPIO  # type: ignore[import-not-found]

        self._gpio = GPIO
        self.pin = pin
        self.active_high = active_high
        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BOARD)
        GPIO.setup(pin, GPIO.OUT, initial=self._level(True))  # radio on until told otherwise
        self.state = True

    def _level(self, on: bool) -> int:
        high, low = self._gpio.HIGH, self._gpio.LOW
        return (high if on else low) if self.active_high else (low if on else high)

    def set(self, on: bool) -> None:
        self._gpio.output(self.pin, self._level(on))
        self.state = on

    def close(self) -> None:
        try:
            self._gpio.cleanup(self.pin)
        except Exception as exc:  # pragma: no cover - hardware teardown
            log.warning("gpio cleanup failed: %s", exc)


def make_relay(pin: int, active_high: bool, *, mock: bool) -> Relay:
    if mock:
        return MockRelay()
    try:
        return JetsonGpioRelay(pin, active_high)
    except Exception as exc:  # ImportError, RuntimeError (not a Jetson), permissions
        log.warning("Jetson.GPIO unavailable (%s): air-gap relay is a no-op mock", exc)
        return MockRelay()


@dataclass
class Override:
    on: bool
    reason: str
    until: float


class AirGap:
    def __init__(self, relay: Relay, mode: str = "off", grace_s: float = 60.0) -> None:
        self.relay = relay
        self._mode: str = mode if mode in MODES else "off"
        self.grace_s = grace_s
        self._owner_last_ts = -math.inf
        self._override: Override | None = None
        self.network: bool | None = None  # last applied state

    # -- inputs -------------------------------------------------------------------------
    @property
    def mode(self) -> str:
        return self._mode

    @mode.setter
    def mode(self, value: str) -> None:
        if value not in MODES:
            raise ValueError(f"airgap mode must be one of {MODES}, got {value!r}")
        if value != self._mode:
            log.info("air-gap mode %s -> %s", self._mode, value)
        self._mode = value

    def owner_seen(self, ts: float) -> None:
        self._owner_last_ts = ts

    def override(self, on: bool, reason: str, for_secs: float, now: float) -> bool:
        """Daemon request. Returns True if it will have an effect in the current mode."""
        for_secs = max(0.0, float(for_secs))
        if self._mode == "off" or (self._mode == "strict" and on):
            log.info("network %s (%s) ignored in mode %s", on, reason, self._mode)
            return False
        self._override = Override(on=on, reason=reason or "", until=now + for_secs)
        log.info("network override on=%s reason=%s for %.0fs", on, reason, for_secs)
        return True

    def clear_override(self) -> None:
        self._override = None

    @property
    def active_override(self) -> Override | None:
        return self._override

    # -- decision -------------------------------------------------------------------------
    def desired(self, now: float) -> bool:
        if self._mode == "off":
            return True
        ov = self._override
        if ov is not None and now >= ov.until:
            self._override = ov = None
        owner_recent = now - self._owner_last_ts <= self.grace_s
        if ov is not None:
            if not ov.on:
                return False  # explicit "radio off" wins in strict and night
            if self._mode == "night":
                return True
        return owner_recent

    def evaluate(self, now: float) -> bool:
        want = self.desired(now)
        if want != self.network:
            try:
                self.relay.set(want)
            except Exception as exc:
                log.error("relay switch failed: %s", exc)
            self.network = want
            log.info("radio %s", "ON" if want else "OFF")
        return want

    def close(self) -> None:
        # Leave the radio powered when the service exits: an unreachable robot after a
        # crash is worse than a few minutes online.
        try:
            self.relay.set(True)
        finally:
            self.relay.close()
