"""Servo health poll, cooldown gating and the short-reply guard on the bus."""

import struct

import pytest

from pincer_robot.body import (
    Body,
    BodyBackend,
    BodyGeometry,
    Bus,
    BusActuator,
    GaitParams,
    LoopbackTransport,
    ServoError,
    ServoHealth,
)
from pincer_robot.body import servo as sv
from pincer_robot.motion import RobotStatus


def _bus_with(ids: list[int], temp: int, load: int) -> tuple[Bus, LoopbackTransport]:
    io = LoopbackTransport()
    for sid in ids:
        io.queue_reply(sid, bytes([temp]))
        io.queue_reply(sid, struct.pack("<H", load))
    return Bus(io), io


def test_short_reply_is_a_servo_error_not_a_struct_error() -> None:
    io = LoopbackTransport()
    io.queue_reply(3, b"\x10")  # one byte where position needs two
    bus = Bus(io)
    with pytest.raises(ServoError, match="short reply"):
        bus.position(3)
    io.queue_reply(3, b"\x10")
    with pytest.raises(ServoError, match="short reply"):
        bus.load(3)
    io.queue_reply(3, b"")
    with pytest.raises(ServoError, match="short reply"):
        bus.temperature(3)
    # a truncated frame on the wire is also a ServoError
    io.replies.append(sv.HEADER + bytes([3, 4, 0, 1]))
    with pytest.raises(ServoError, match="short frame"):
        bus.position(3)


def test_health_report_within_limits() -> None:
    ids = [1, 2, 3]
    bus, _ = _bus_with(ids, temp=41, load=120)
    h = ServoHealth(bus, ids, max_temp_c=70, max_load=800, clock=lambda: 100.0)
    r = h.check()
    assert r.ok and r.hottest_c == 41 and r.heaviest_load == 120 and not r.unreadable
    assert not h.in_cooldown()


def test_overheat_and_overload_start_a_cooldown_and_block_driving() -> None:
    ids = [1, 2]
    io = LoopbackTransport()
    io.queue_reply(1, bytes([74]))  # hot
    io.queue_reply(1, struct.pack("<H", 100))
    io.queue_reply(2, bytes([40]))
    io.queue_reply(2, struct.pack("<H", 0x400 | 950))  # negative, heavy
    now = [100.0]
    h = ServoHealth(Bus(io), ids, max_temp_c=70, max_load=800, cooldown_s=30, clock=lambda: now[0])
    r = h.check()
    assert not r.ok and r.over == ["servo 1 at 74 C", "servo 2 load 950"]
    assert h.in_cooldown()

    act = BusActuator(Bus(io), BodyGeometry())
    body = Body(geometry=BodyGeometry(), gait=GaitParams(), actuator=act)
    backend = BodyBackend(body, RobotStatus(), health=h)
    assert backend.resting() == "servo 1 at 74 C; servo 2 load 950"
    before = len(io.frames)
    backend._drive(1.0, 0.0, 0.2)
    assert len(io.frames) == before  # no sync-write went out
    assert backend.get_status().is_moving is False

    now[0] = 131.0  # cooldown over
    assert backend.resting() is None


def test_unreadable_servos_are_reported_not_fatal() -> None:
    ids = [5, 6]
    bus, _ = _bus_with([5], temp=50, load=10)  # 6 never answers
    h = ServoHealth(bus, ids)
    r = h.check()
    assert r.ok and r.unreadable == [6] and r.hottest_c == 50


def test_sensor_status_is_shared_and_shutdown_reaches_sensors() -> None:
    class Sensors:
        def __init__(self) -> None:
            self.down = False

        def shutdown(self) -> None:
            self.down = True

    status = RobotStatus(note="body: own gait")
    s = Sensors()
    body = Body(geometry=BodyGeometry(), gait=GaitParams(), actuator=None)
    backend = BodyBackend(body, status, sensors=s)
    status.tilt_deg = 50.0  # what a SensorNode writes from the IMU
    status.front_clearance_m = 0.1
    st = backend.get_status()
    assert st.tilt_deg == 50.0 and st.front_clearance_m == 0.1
    assert backend.walk("forward", 0.5).startswith("REFUSED: body tilted")
    backend.shutdown()
    assert s.down
