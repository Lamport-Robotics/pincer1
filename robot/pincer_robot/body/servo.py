"""Serial-bus servo driver for the legs: the half-duplex TTL protocol the STS3215 class speaks.

Packet: ``FF FF id len instr params... chk`` with ``len = len(params) + 2`` and
``chk = ~(id + len + instr + sum(params)) & 0xFF``. Replies carry an error byte in place of the
instruction. Positions are 12-bit (0..4095 over a full turn, 2048 at the calibrated middle),
multi-byte
values little-endian. ``SyncWrite`` moves every joint in one packet, which is what a gait needs.

Nothing here opens a port by itself: pass a ``Transport`` (``SerialTransport`` for a real adapter,
``LoopbackTransport`` for tests). Calibration (sign and zero per joint) lives in ``Calibration``.
"""

from __future__ import annotations

import math
import struct
import time
from dataclasses import dataclass, field
from typing import Protocol

HEADER = b"\xff\xff"
PING, READ, WRITE, REG_WRITE, ACTION, SYNC_WRITE = 0x01, 0x02, 0x03, 0x04, 0x05, 0x83
BROADCAST = 0xFE

# registers (STS series memory table)
REG_TORQUE_ENABLE = 0x28
REG_ACCELERATION = 0x29
REG_GOAL_POSITION = 0x2A
REG_GOAL_TIME = 0x2C
REG_GOAL_SPEED = 0x2E
REG_LOCK = 0x37
REG_PRESENT_POSITION = 0x38
REG_PRESENT_SPEED = 0x3A
REG_PRESENT_LOAD = 0x3C
REG_PRESENT_VOLTAGE = 0x3E
REG_PRESENT_TEMPERATURE = 0x3F
REG_MOVING = 0x42
REG_PRESENT_CURRENT = 0x45

TICKS_PER_TURN = 4096
CENTRE = 2048


class ServoError(RuntimeError):
    pass


def checksum(payload: bytes) -> int:
    return (~sum(payload)) & 0xFF


def build(servo_id: int, instruction: int, params: bytes = b"") -> bytes:
    if not 0 <= servo_id <= 0xFE:
        raise ValueError(f"servo id {servo_id} out of range")
    body = bytes([servo_id, len(params) + 2, instruction]) + params
    return HEADER + body + bytes([checksum(body)])


@dataclass(frozen=True)
class Reply:
    servo_id: int
    error: int
    params: bytes


def parse(frame: bytes) -> Reply:
    """Parse one status packet; raises ``ServoError`` on a bad frame."""
    if len(frame) < 6 or frame[:2] != HEADER:
        raise ServoError(f"bad header in {frame.hex()}")
    servo_id, length, error = frame[2], frame[3], frame[4]
    end = 3 + length  # index of the checksum: len counts error, params and checksum
    if len(frame) < end + 1:
        raise ServoError(f"short frame {frame.hex()}")
    params = frame[5:end]
    if checksum(frame[2:end]) != frame[end]:
        raise ServoError(f"checksum mismatch in {frame.hex()}")
    return Reply(servo_id, error, bytes(params))


def sync_write(address: int, rows: list[tuple[int, bytes]]) -> bytes:
    if not rows:
        raise ValueError("nothing to write")
    width = len(rows[0][1])
    if any(len(data) != width for _, data in rows):
        raise ValueError("sync write rows must have one width")
    params = bytes([address, width]) + b"".join(bytes([sid]) + data for sid, data in rows)
    return build(BROADCAST, SYNC_WRITE, params)


def u16(value: int) -> bytes:
    return struct.pack("<H", max(0, min(0xFFFF, int(value))))


class Transport(Protocol):
    def write(self, data: bytes) -> None: ...
    def read(self, n: int, timeout_s: float) -> bytes: ...


class SerialTransport:
    """A UART adapter on a tty (pyserial). 1 Mbaud is the usual bus speed."""

    def __init__(self, port: str, baud: int = 1_000_000):
        import serial  # optional dependency, only on the robot

        self._ser = serial.Serial(port, baud, timeout=0)

    def write(self, data: bytes) -> None:
        self._ser.reset_input_buffer()
        self._ser.write(data)
        self._ser.flush()

    def read(self, n: int, timeout_s: float) -> bytes:
        deadline = time.monotonic() + timeout_s
        out = b""
        while len(out) < n and time.monotonic() < deadline:
            chunk = self._ser.read(n - len(out))
            if chunk:
                out += chunk
            else:
                time.sleep(0.0005)
        return out


class LoopbackTransport:
    """Records every frame and answers reads from a scripted queue (tests, dry runs)."""

    def __init__(self) -> None:
        self.frames: list[bytes] = []
        self.replies: list[bytes] = []

    def write(self, data: bytes) -> None:
        self.frames.append(data)

    def read(self, n: int, timeout_s: float) -> bytes:
        if not self.replies:
            return b""
        head = self.replies.pop(0)
        return head[:n]

    def queue_reply(self, servo_id: int, params: bytes = b"", error: int = 0) -> None:
        body = bytes([servo_id, len(params) + 2, error]) + params
        self.replies.append(HEADER + body + bytes([checksum(body)]))


@dataclass
class Calibration:
    """Per-servo sign and zero: ticks = centre + sign * radians * k + offset."""

    sign: dict[int, int] = field(default_factory=dict)
    offset_ticks: dict[int, int] = field(default_factory=dict)

    def to_ticks(self, servo_id: int, radians: float) -> int:
        k = TICKS_PER_TURN / (2 * math.pi)
        ticks = (
            CENTRE + self.sign.get(servo_id, 1) * radians * k + self.offset_ticks.get(servo_id, 0)
        )
        return max(0, min(TICKS_PER_TURN - 1, int(round(ticks))))

    def to_radians(self, servo_id: int, ticks: int) -> float:
        k = (2 * math.pi) / TICKS_PER_TURN
        return (
            (ticks - CENTRE - self.offset_ticks.get(servo_id, 0)) * self.sign.get(servo_id, 1) * k
        )


class Bus:
    """One serial line, many servos."""

    def __init__(
        self,
        transport: Transport,
        calibration: Calibration | None = None,
        reply_timeout_s: float = 0.02,
    ):
        self.io = transport
        self.cal = calibration or Calibration()
        self.timeout = reply_timeout_s

    def _ask(self, servo_id: int, instruction: int, params: bytes, reply_params: int) -> Reply:
        self.io.write(build(servo_id, instruction, params))
        frame = self.io.read(6 + reply_params, self.timeout)
        if not frame:
            raise ServoError(f"servo {servo_id}: no reply")
        reply = parse(frame)
        if reply.servo_id != servo_id:
            raise ServoError(f"servo {servo_id}: reply from {reply.servo_id}")
        if len(reply.params) != reply_params:
            raise ServoError(
                f"servo {servo_id}: short reply, {len(reply.params)} of {reply_params} bytes"
            )
        return reply

    def ping(self, servo_id: int) -> bool:
        try:
            return self._ask(servo_id, PING, b"", 0).error == 0
        except ServoError:
            return False

    def read(self, servo_id: int, address: int, length: int) -> bytes:
        return self._ask(servo_id, READ, bytes([address, length]), length).params

    def write(self, servo_id: int, address: int, data: bytes) -> None:
        self._ask(servo_id, WRITE, bytes([address]) + data, 0)

    def torque(self, servo_ids: list[int], on: bool) -> None:
        self.io.write(
            sync_write(REG_TORQUE_ENABLE, [(sid, bytes([1 if on else 0])) for sid in servo_ids])
        )

    def move(self, targets: dict[int, float], time_ms: int = 0, speed: int = 0) -> None:
        """Send every joint its goal position (radians) in one packet, with the same timing."""
        rows = [
            (sid, u16(self.cal.to_ticks(sid, rad)) + u16(time_ms) + u16(speed))
            for sid, rad in sorted(targets.items())
        ]
        self.io.write(sync_write(REG_GOAL_POSITION, rows))

    def position(self, servo_id: int) -> float:
        raw = self.read(servo_id, REG_PRESENT_POSITION, 2)
        return self.cal.to_radians(servo_id, struct.unpack("<H", raw)[0])

    def temperature(self, servo_id: int) -> int:
        return self.read(servo_id, REG_PRESENT_TEMPERATURE, 1)[0]

    def voltage(self, servo_id: int) -> float:
        return self.read(servo_id, REG_PRESENT_VOLTAGE, 1)[0] / 10.0

    def load(self, servo_id: int) -> int:
        """Signed load, -1000..1000 (bit 10 is the sign)."""
        raw = struct.unpack("<H", self.read(servo_id, REG_PRESENT_LOAD, 2))[0]
        return -(raw & 0x3FF) if raw & 0x400 else raw & 0x3FF
