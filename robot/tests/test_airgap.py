import sys
import types

import pytest

from pincer_robot.airgap import AirGap, JetsonGpioRelay, MockRelay, make_relay


def _gap(mode: str, grace: float = 60.0) -> tuple[AirGap, MockRelay]:
    relay = MockRelay()
    return AirGap(relay, mode=mode, grace_s=grace), relay


def test_off_mode_radio_always_on_and_ignores_overrides() -> None:
    gap, relay = _gap("off")
    assert gap.evaluate(0.0) is True
    assert gap.override(False, "broadcast", 600, 0.0) is False
    assert gap.evaluate(1.0) is True
    assert gap.evaluate(5000.0) is True
    assert relay.changes == [True]  # only applied once


def test_strict_follows_owner_with_grace() -> None:
    gap, relay = _gap("strict", grace=60.0)
    assert gap.evaluate(0.0) is False  # nobody seen yet
    gap.owner_seen(10.0)
    assert gap.evaluate(10.0) is True
    assert gap.evaluate(69.0) is True  # inside the 60 s grace
    assert gap.evaluate(70.5) is False
    gap.owner_seen(80.0)
    assert gap.evaluate(80.0) is True
    assert relay.changes == [False, True, False, True]


def test_strict_ignores_on_request_but_honours_off() -> None:
    gap, _ = _gap("strict")
    assert gap.override(True, "agent_window", 600, 0.0) is False
    assert gap.evaluate(0.0) is False
    gap.owner_seen(100.0)
    assert gap.evaluate(100.0) is True
    assert gap.override(False, "broadcast", 30, 100.0) is True
    assert gap.evaluate(101.0) is False  # owner present, but the daemon said off
    assert gap.evaluate(131.0) is True  # override expired, owner still inside the grace


def test_night_honours_window_with_expiry() -> None:
    gap, relay = _gap("night", grace=60.0)
    assert gap.evaluate(0.0) is False
    assert gap.override(True, "agent_window", 600, 0.0) is True
    assert gap.evaluate(1.0) is True
    assert gap.evaluate(599.0) is True
    assert gap.evaluate(600.0) is False  # expired exactly at the deadline
    assert gap.active_override is None
    gap.owner_seen(700.0)
    assert gap.evaluate(700.0) is True
    assert relay.changes == [False, True, False, True]


def test_night_off_request_wins_over_owner() -> None:
    gap, _ = _gap("night")
    gap.owner_seen(0.0)
    assert gap.evaluate(0.0) is True
    gap.override(False, "broadcast_done", 10, 0.0)
    assert gap.evaluate(1.0) is False
    assert gap.evaluate(11.0) is True


def test_mode_switch_applies_immediately_and_validates() -> None:
    gap, relay = _gap("off")
    assert gap.evaluate(0.0) is True
    gap.mode = "strict"
    assert gap.evaluate(0.1) is False
    gap.mode = "off"
    assert gap.evaluate(0.2) is True
    with pytest.raises(ValueError):
        gap.mode = "sometimes"
    assert relay.changes == [True, False, True]


def test_close_leaves_radio_on() -> None:
    gap, relay = _gap("strict")
    gap.evaluate(0.0)
    assert relay.state is False
    gap.close()
    assert relay.state is True


def test_jetson_relay_uses_board_pin_and_polarity(monkeypatch) -> None:  # noqa: ANN001
    calls: list[tuple] = []

    fake = types.ModuleType("Jetson.GPIO")
    fake.BOARD, fake.OUT, fake.HIGH, fake.LOW = "BOARD", "OUT", 1, 0  # type: ignore[attr-defined]
    fake.setwarnings = lambda flag: calls.append(("warn", flag))  # type: ignore[attr-defined]
    fake.setmode = lambda mode: calls.append(("mode", mode))  # type: ignore[attr-defined]
    fake.setup = lambda pin, d, initial: calls.append(("setup", pin, d, initial))  # type: ignore[attr-defined]
    fake.output = lambda pin, level: calls.append(("out", pin, level))  # type: ignore[attr-defined]
    fake.cleanup = lambda pin: calls.append(("cleanup", pin))  # type: ignore[attr-defined]
    pkg = types.ModuleType("Jetson")
    pkg.GPIO = fake  # type: ignore[attr-defined]
    monkeypatch.setitem(sys.modules, "Jetson", pkg)
    monkeypatch.setitem(sys.modules, "Jetson.GPIO", fake)

    relay = JetsonGpioRelay(pin=7, active_high=False)
    assert ("mode", "BOARD") in calls
    assert ("setup", 7, "OUT", 0) in calls  # active-low: radio on = LOW at boot
    relay.set(False)
    assert calls[-1] == ("out", 7, 1)
    relay.set(True)
    assert calls[-1] == ("out", 7, 0)
    relay.close()
    assert calls[-1] == ("cleanup", 7)

    hi = make_relay(11, True, mock=False)
    assert isinstance(hi, JetsonGpioRelay) and ("setup", 11, "OUT", 1) in calls


def test_make_relay_falls_back_to_mock_without_gpio(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.setitem(sys.modules, "Jetson", None)
    monkeypatch.setitem(sys.modules, "Jetson.GPIO", None)
    assert isinstance(make_relay(7, True, mock=False), MockRelay)
    assert isinstance(make_relay(7, True, mock=True), MockRelay)
