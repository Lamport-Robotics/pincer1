import sys
import types

import pytest

from pincer_robot.battery import (
    OCV_3S,
    PACK_EMPTY_V,
    PACK_FULL_V,
    BatteryMonitor,
    BatteryState,
    Ina226Sensor,
    MockBatterySensor,
    make_battery_sensor,
    soc_from_voltage,
    voltage_from_soc,
)


def test_ocv_table_monotonic_and_endpoints() -> None:
    volts = [v for v, _ in OCV_3S]
    pcts = [p for _, p in OCV_3S]
    assert volts == sorted(volts) and len(set(volts)) == len(volts)
    assert pcts == sorted(pcts) and pcts[0] == 0.0 and pcts[-1] == 100.0
    assert PACK_EMPTY_V == 9.0 and PACK_FULL_V == 12.6
    assert soc_from_voltage(9.0) == 0.0 and soc_from_voltage(12.6) == 100.0
    assert soc_from_voltage(8.0) == 0.0 and soc_from_voltage(13.0) == 100.0
    assert soc_from_voltage(float("nan")) == 0.0
    # Strictly increasing through the whole range, 10 mV steps.
    prev = -1.0
    for i in range(900, 1261):
        p = soc_from_voltage(i / 100.0)
        assert p >= prev
        prev = p
    assert 25.0 <= soc_from_voltage(11.1) <= 50.0  # nominal sits in the flat middle
    # Interpolation lands between table points and inverts.
    for v, p in OCV_3S:
        assert soc_from_voltage(v) == pytest.approx(p)
        assert voltage_from_soc(p) == pytest.approx(v)
    mid = (OCV_3S[5][0] + OCV_3S[6][0]) / 2
    assert soc_from_voltage(mid) == pytest.approx((OCV_3S[5][1] + OCV_3S[6][1]) / 2)
    assert voltage_from_soc(-5) == 9.0 and voltage_from_soc(150) == 12.6


def test_charging_detection_threshold_and_inversion() -> None:
    sensor = MockBatterySensor(11.5, 0.29)
    mon = BatteryMonitor(sensor, tau_s=0.0, clock=lambda: 0.0)
    assert mon.read().charging is False
    sensor.set(current_a=0.3)
    assert mon.read().charging is True
    sensor.set(current_a=2.8)
    st = mon.read()
    assert st.charging and st.current_a == 2.8
    sensor.set(current_a=-4.0)  # walking
    assert mon.read().charging is False
    inv = BatteryMonitor(MockBatterySensor(11.5, -2.0), tau_s=0.0, invert_current=True)
    st = inv.read()
    assert st.charging is True and st.current_a == 2.0


def test_voltage_low_pass_two_seconds() -> None:
    sensor = MockBatterySensor(11.0, -1.0)
    t = {"now": 0.0}
    mon = BatteryMonitor(sensor, tau_s=2.0, clock=lambda: t["now"])
    assert mon.read().voltage == 11.0  # first sample seeds the filter
    sensor.set(voltage=12.0)
    t["now"] = 2.0
    st = mon.read()
    assert 11.6 < st.voltage < 11.66  # 1 - e^-1 of the step after one time constant
    t["now"] = 20.0
    assert mon.read().voltage == pytest.approx(12.0, abs=1e-3)
    assert mon.read(now=20.0).pct == pytest.approx(soc_from_voltage(12.0), abs=0.05)


def test_sensor_errors_keep_last_state() -> None:
    sensor = MockBatterySensor(11.4, -0.5)
    mon = BatteryMonitor(sensor, tau_s=0.0, clock=lambda: 1.0)
    good = mon.read()
    sensor.fail = True
    assert mon.read() is good and mon.errors == 1
    fresh = BatteryMonitor(MockBatterySensor(fail := 11.0), tau_s=0.0)
    fresh.sensor.fail = True  # type: ignore[attr-defined]
    st = fresh.read()
    assert st == BatteryState(0.0, 0.0, 0.0, False, st.ts) and fail == 11.0


def test_mock_pack_drains_and_charges_on_a_slope() -> None:
    t = {"now": 0.0}
    sensor = MockBatterySensor.at_pct(50.0, slope_v_per_s=-0.01, clock=lambda: t["now"])
    v0 = sensor.read_raw()[0]
    t["now"] = 10.0
    assert sensor.read_raw()[0] == pytest.approx(v0 - 0.1)
    sensor.slope_v_per_s = +1.0
    t["now"] = 100.0
    assert sensor.read_raw()[0] == PACK_FULL_V  # clamped
    sensor.set_pct(20.0)
    assert soc_from_voltage(sensor.voltage) == pytest.approx(20.0)
    mock = make_battery_sensor(mock=True, mock_pct=30.0, mock_drain_s=600.0)
    assert isinstance(mock, MockBatterySensor) and mock.slope_v_per_s < 0
    assert BatteryState(11.234, -0.5, 42.6, False, 0).to_message() == {
        "pct": 43, "voltage": 11.23, "charging": False
    }  # fmt: skip


class _FakeBus:
    def __init__(self, regs: dict[int, int]) -> None:
        self.regs = regs
        self.writes: list[tuple[int, int, int]] = []
        self.closed = False

    @staticmethod
    def _swap(w: int) -> int:
        return ((w & 0xFF) << 8) | (w >> 8)

    def read_word_data(self, addr: int, reg: int) -> int:
        return self._swap(self.regs.get(reg, 0))  # the chip is big-endian, smbus little

    def write_word_data(self, addr: int, reg: int, value: int) -> None:
        self.writes.append((addr, reg, value))
        self.regs[reg] = self._swap(value)

    def close(self) -> None:
        self.closed = True


def test_ina226_register_decoding_with_fake_smbus(monkeypatch) -> None:  # noqa: ANN001
    buses: list[_FakeBus] = []

    def smbus(bus: int) -> _FakeBus:
        # 11.52 V on the bus (9216 LSB of 1.25 mV); -3.0 A through 2 mOhm = -6 mV = -2400 LSB.
        b = _FakeBus({0x02: 9216, 0x01: (-2400) & 0xFFFF, 0xFF: 0x2260})
        buses.append(b)
        return b

    fake = types.ModuleType("smbus2")
    fake.SMBus = smbus  # type: ignore[attr-defined]
    monkeypatch.setitem(sys.modules, "smbus2", fake)
    s = Ina226Sensor(bus=7, addr=0x40, shunt_ohm=0.002)
    assert buses[0].writes == [(0x40, 0x00, _FakeBus._swap(0x4527))]
    v, i = s.read_raw()
    assert v == pytest.approx(11.52) and i == pytest.approx(-3.0)
    buses[0].regs[0x01] = 1200  # +1.5 A into the pack
    assert s.read_raw()[1] == pytest.approx(1.5)
    s.close()
    assert buses[0].closed
    with pytest.raises(ValueError):
        Ina226Sensor(shunt_ohm=0)
    # Real factory falls back to the mock when the chip is not there.
    fake.SMBus = lambda bus: (_ for _ in ()).throw(OSError("no /dev/i2c-7"))  # type: ignore[attr-defined]
    assert isinstance(make_battery_sensor(mock=False), MockBatterySensor)


def test_make_battery_sensor_without_smbus2_falls_back(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.setitem(sys.modules, "smbus2", None)  # import raises ImportError
    assert isinstance(make_battery_sensor(mock=False), MockBatterySensor)
