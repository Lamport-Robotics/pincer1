"""Pack monitor: voltage, current, state of charge and "is it charging".

Hardware (docs/HARDWARE.md, README "Charging dock"): a 3S4P Li-ion 21700 pack (9.0 V cutoff,
11.1 V nominal, 12.6 V full; the BMS balances) measured by a TI INA226 on the pack rail,
I2C to the Jetson (address 0x40, 2 mOhm shunt). The INA226 is read raw: the shunt-voltage
register (LSB 2.5 uV) divided by the shunt resistance gives the current, the bus-voltage
register (LSB 1.25 mV) the pack voltage; no calibration register is needed for that.

Conventions:

* ``current_a`` is positive when current flows INTO the pack (charging), negative while
  discharging. Which way round that comes out depends on which side of the shunt the
  pack is on; ``invert_current`` fixes a backwards shunt without rewiring.
* ``charging`` is ``current_a >= charge_threshold_a`` (0.3 A). A plate that merely touches
  the pogo pins without a charger behind it never shows current, so this is also the
  dock's "contact made" signal.
* ``pct`` comes from a 3S open-circuit-voltage table (``OCV_3S``), linearly interpolated.
  **It is approximate under load**: walking pulls the terminal voltage down (a few hundred
  mV at 5-8 A on this pack), so the number dips while moving and recovers at rest. The
  2 s low-pass on the voltage removes servo spikes, not the sag. A coulomb counter on
  the same INA226 (it has a power register and we already read the current) can be added
  later for a load-independent estimate; the OCV table then only re-anchors it at rest.

``MockBatterySensor`` is a scriptable pack for tests and ``--mock``: set the voltage and
current directly, or give it a slope (V/s) so it drains or charges on its own.
"""

from __future__ import annotations

import logging
import math
import time
from bisect import bisect_right
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Protocol

log = logging.getLogger("pincer.battery")

# Per-cell open-circuit voltage -> state of charge for a high-energy 21700 (P42A / 50E
# class), scaled to 3S. Rest voltages; the curve is flat between 20 and 80 %, which is
# why a coulomb counter is the better instrument there.
OCV_CELL: tuple[tuple[float, float], ...] = (
    (3.00, 0.0),
    (3.30, 5.0),
    (3.45, 10.0),
    (3.55, 20.0),
    (3.62, 30.0),
    (3.68, 40.0),
    (3.75, 50.0),
    (3.83, 60.0),
    (3.90, 70.0),
    (3.98, 80.0),
    (4.08, 90.0),
    (4.20, 100.0),
)
OCV_3S: tuple[tuple[float, float], ...] = tuple((round(v * 3, 3), p) for v, p in OCV_CELL)
PACK_EMPTY_V = OCV_3S[0][0]  # 9.0
PACK_FULL_V = OCV_3S[-1][0]  # 12.6
PACK_NOMINAL_V = 11.1

CHARGE_THRESHOLD_A = 0.3

_OCV_V = [v for v, _ in OCV_3S]
_OCV_P = [p for _, p in OCV_3S]


def soc_from_voltage(voltage: float) -> float:
    """Pack voltage (3S, at rest) -> state of charge 0..100, linear between table points."""
    if not math.isfinite(voltage):
        return 0.0
    if voltage <= _OCV_V[0]:
        return 0.0
    if voltage >= _OCV_V[-1]:
        return 100.0
    i = bisect_right(_OCV_V, voltage)
    v0, v1 = _OCV_V[i - 1], _OCV_V[i]
    p0, p1 = _OCV_P[i - 1], _OCV_P[i]
    return p0 + (p1 - p0) * (voltage - v0) / (v1 - v0)


def voltage_from_soc(pct: float) -> float:
    """Inverse of ``soc_from_voltage`` (for mocks and tests)."""
    pct = max(0.0, min(100.0, pct))
    i = bisect_right(_OCV_P, pct)
    if i >= len(_OCV_P):
        return _OCV_V[-1]
    p0, p1 = _OCV_P[i - 1], _OCV_P[i]
    v0, v1 = _OCV_V[i - 1], _OCV_V[i]
    return v0 + (v1 - v0) * (pct - p0) / (p1 - p0)


@dataclass(frozen=True)
class BatteryState:
    voltage: float  # filtered pack voltage, V
    current_a: float  # + into the pack (charging), - out of it
    pct: float  # 0..100 from the OCV table (approximate under load)
    charging: bool
    ts: float

    def to_message(self) -> dict[str, Any]:
        """The ``battery`` object inside ``presence`` (PROTOCOL.md section 2)."""
        return {
            "pct": int(round(self.pct)),
            "voltage": round(self.voltage, 2),
            "charging": self.charging,
        }


class BatterySensor(Protocol):
    def read_raw(self) -> tuple[float, float]:
        """(pack voltage V, current A with + = into the pack). May raise on I/O errors."""
        ...

    def close(self) -> None: ...


class MockBatterySensor:
    """A pack you can script: fixed values, or a voltage slope so it drains/charges."""

    def __init__(
        self,
        voltage: float = 11.7,
        current_a: float = -0.9,
        *,
        slope_v_per_s: float = 0.0,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.voltage = float(voltage)
        self.current_a = float(current_a)
        self.slope_v_per_s = float(slope_v_per_s)
        self._clock = clock
        self._last = clock()
        self.fail = False  # raise OSError on the next read (bus error simulation)

    @classmethod
    def at_pct(cls, pct: float, current_a: float = -0.9, **kw: Any) -> MockBatterySensor:
        return cls(voltage_from_soc(pct), current_a, **kw)

    def set(self, voltage: float | None = None, current_a: float | None = None) -> None:
        if voltage is not None:
            self.voltage = float(voltage)
        if current_a is not None:
            self.current_a = float(current_a)

    def set_pct(self, pct: float) -> None:
        self.voltage = voltage_from_soc(pct)

    def read_raw(self) -> tuple[float, float]:
        if self.fail:
            raise OSError("mock bus error")
        now = self._clock()
        dt = max(0.0, now - self._last)
        self._last = now
        if self.slope_v_per_s:
            v = self.voltage + self.slope_v_per_s * dt
            self.voltage = max(PACK_EMPTY_V - 0.2, min(PACK_FULL_V, v))
        return self.voltage, self.current_a

    def close(self) -> None:
        pass


class Ina226Sensor:
    """TI INA226 over ``smbus2`` (imported here, only on the robot).

    Registers (16-bit, big-endian on the wire; smbus2 word reads are little-endian, so the
    bytes are swapped): 0x00 config, 0x01 shunt voltage (signed, 2.5 uV/LSB), 0x02 bus
    voltage (1.25 mV/LSB). Config 0x4527 = continuous shunt+bus, 1.1 ms conversions,
    16-sample averaging (the default 0x4127 with averaging raised from 1 to 16).
    """

    REG_CONFIG = 0x00
    REG_SHUNT = 0x01
    REG_BUS = 0x02
    REG_ID = 0xFF
    CONFIG_WORD = 0x4527
    SHUNT_LSB_V = 2.5e-6
    BUS_LSB_V = 1.25e-3

    def __init__(self, bus: int = 7, addr: int = 0x40, shunt_ohm: float = 0.002) -> None:
        import smbus2  # type: ignore[import-not-found]

        if shunt_ohm <= 0:
            raise ValueError("shunt_ohm must be positive")
        self.addr = addr
        self.shunt_ohm = shunt_ohm
        self._bus = smbus2.SMBus(bus)
        self._write(self.REG_CONFIG, self.CONFIG_WORD)
        ident = self._read(self.REG_ID)
        if ident not in (0x2260, 0x0000, 0xFFFF):  # die id; some clones answer 0
            log.warning("INA226 at 0x%02x on bus %d answered id 0x%04x", addr, bus, ident)
        log.info("INA226 on /dev/i2c-%d addr 0x%02x, shunt %.4f ohm", bus, addr, shunt_ohm)

    @staticmethod
    def _swap(word: int) -> int:
        return ((word & 0xFF) << 8) | ((word >> 8) & 0xFF)

    def _read(self, reg: int) -> int:
        return self._swap(self._bus.read_word_data(self.addr, reg))

    def _write(self, reg: int, value: int) -> None:
        self._bus.write_word_data(self.addr, reg, self._swap(value & 0xFFFF))

    def read_raw(self) -> tuple[float, float]:
        shunt = self._read(self.REG_SHUNT)
        if shunt >= 0x8000:
            shunt -= 0x10000
        bus_v = self._read(self.REG_BUS) * self.BUS_LSB_V
        current = shunt * self.SHUNT_LSB_V / self.shunt_ohm
        return bus_v, current

    def close(self) -> None:
        try:
            self._bus.close()
        except Exception as exc:  # pragma: no cover - hardware teardown
            log.warning("i2c close failed: %s", exc)


def make_battery_sensor(
    *,
    mock: bool,
    bus: int = 7,
    addr: int = 0x40,
    shunt_ohm: float = 0.002,
    mock_pct: float = 55.0,
    mock_drain_s: float = 1800.0,
) -> BatterySensor:
    """INA226 on the robot; a slowly draining mock on a laptop (or when the bus is absent)."""
    if not mock:
        try:
            return Ina226Sensor(bus, addr, shunt_ohm)
        except Exception as exc:  # ImportError (no smbus2), OSError (no bus / no chip)
            log.warning("INA226 unavailable (%s): battery is a mock pack", exc)
    slope = -(PACK_FULL_V - PACK_EMPTY_V) / max(60.0, mock_drain_s)
    return MockBatterySensor.at_pct(mock_pct, slope_v_per_s=slope)


class BatteryMonitor:
    """Reads the sensor, low-passes the voltage (``tau_s``) and derives pct / charging."""

    def __init__(
        self,
        sensor: BatterySensor,
        *,
        tau_s: float = 2.0,
        charge_threshold_a: float = CHARGE_THRESHOLD_A,
        invert_current: bool = False,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self.sensor = sensor
        self.tau_s = max(0.0, tau_s)
        self.charge_threshold_a = charge_threshold_a
        self.invert_current = invert_current
        self._clock = clock
        self._v: float | None = None
        self._ts: float = -math.inf
        self.last: BatteryState | None = None
        self.errors = 0

    def read(self, now: float | None = None) -> BatteryState:
        """One sample. On a sensor error the previous state is returned (``errors`` counts)."""
        ts = self._clock() if now is None else now
        try:
            raw_v, raw_i = self.sensor.read_raw()
        except Exception as exc:
            self.errors += 1
            if self.errors in (1, 10, 100) or self.errors % 1000 == 0:
                log.error("battery read failed (%d): %s", self.errors, exc)
            if self.last is not None:
                return self.last
            state = BatteryState(0.0, 0.0, 0.0, False, ts)
            self.last = state
            return state
        if self.invert_current:
            raw_i = -raw_i
        if self._v is None or self.tau_s <= 0:
            v = float(raw_v)
        else:
            dt = max(0.0, ts - self._ts)
            alpha = 1.0 - math.exp(-dt / self.tau_s) if dt > 0 else 0.0
            v = self._v + alpha * (float(raw_v) - self._v)
        self._v = v
        self._ts = ts
        state = BatteryState(
            voltage=v,
            current_a=float(raw_i),
            pct=soc_from_voltage(v),
            charging=float(raw_i) >= self.charge_threshold_a,
            ts=ts,
        )
        self.last = state
        return state

    def close(self) -> None:
        self.sensor.close()
