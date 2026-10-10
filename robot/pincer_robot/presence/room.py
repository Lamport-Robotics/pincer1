"""Room fingerprint ("room lock"): is the robot standing in the room it was enrolled in?

The fingerprint is built from the lidar *background model* (``PersonDetector``'s
per-beam static ranges after the learning window), never from a live scan with people in
it. It has two parts:

* a **72-bin polar profile** of the static walls, 5 degrees per bin, taken around the
  *polygon centroid* of the static points rather than around the robot. Re-centring on
  the room's own centroid is what gives translation tolerance: a robot that was put down
  0.3-0.5 m away sees the same walls from a different place, but the polygon it traces
  has (almost) the same centroid, so the re-centred profile barely moves.
* the **sorted wall-segment lengths** (the longest 8): consecutive static points are
  joined while the gap is small and the direction does not turn a corner; the lengths of
  those runs, in metres, carry the room's absolute size (the profile is also in metres,
  but the segments are robust to small occlusions and to furniture).

``room_match`` tries all 72 rotations of the profile (the robot may face any way) and
keeps the best; per bin the ranges are compared with a soft tolerance (free up to
``tol_soft`` = 0.10 m, zero credit beyond ``tol_hard`` = 0.35 m). The segment sets are
compared by relative length. Score = 0.9 * profile + 0.1 * segments, in [0, 1]; the
segments are a tie-breaker, the profile does the discriminating (on the synthetic rooms:
same room moved 0.5 m and turned >= 0.90, a 5 x 3 m room against a 4 x 4 m one 0.35, a
room 0.2 m larger on every side 0.60 - that last case is the inherent limit, it looks
like a lidar calibration offset).

Limits (documented, not hidden):

* Translation tolerance is ~0.5 m. Beyond that the robot's own position changes which
  parts of the walls are occluded by furniture, the polygon changes shape, and the
  centroid drifts; the score degrades gradually rather than failing hard.
* Rotation is resolved to 5 degrees (one bin); a sub-bin rotation costs a few percent.
* A room of the same shape *and size* elsewhere (two identical hotel rooms) matches. The
  fingerprint is a lock against "someone carried the robot somewhere else", not an
  identity proof for the room.
* Doors opened/closed and big furniture moves lower the score; re-enrol after redecorating.
* Nothing here is secret: a copy of ``room.json`` does not help an attacker who does not
  physically have the robot in a matching room.
"""

from __future__ import annotations

import json
import math
import os
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

FINGERPRINT_VERSION = 1
PROFILE_BINS = 72
MAX_SEGMENTS = 8


@dataclass
class RoomFingerprint:
    profile: list[float]  # PROFILE_BINS ranges around the centroid; NaN = open / no wall
    segments: list[float]  # wall segment lengths, metres, longest first
    centroid: tuple[float, float]  # where the room centre was, in robot frame at enrolment
    created_ts: float = field(default_factory=time.time)
    version: int = FINGERPRINT_VERSION
    note: str = ""

    def to_json(self) -> str:
        d = asdict(self)
        d["profile"] = [None if (v is None or math.isnan(v)) else round(v, 4) for v in self.profile]
        d["segments"] = [round(v, 4) for v in self.segments]
        return json.dumps(d, indent=1)

    @classmethod
    def from_json(cls, text: str) -> RoomFingerprint:
        d = json.loads(text)
        d["profile"] = [math.nan if v is None else float(v) for v in d["profile"]]
        d["centroid"] = tuple(d.get("centroid", (0.0, 0.0)))
        return cls(**d)


# --- geometry helpers ---------------------------------------------------------------------


def _polygon_centroid(pts: np.ndarray) -> tuple[float, float]:
    """Area centroid (shoelace) of the closed polygon through ``pts`` in angular order.

    Falls back to the point mean when the polygon is degenerate.
    """
    if len(pts) < 3:
        return (float(pts[:, 0].mean()), float(pts[:, 1].mean())) if len(pts) else (0.0, 0.0)
    x, y = pts[:, 0], pts[:, 1]
    xn, yn = np.roll(x, -1), np.roll(y, -1)
    cross = x * yn - xn * y
    area = 0.5 * cross.sum()
    if abs(area) < 1e-6:
        return float(x.mean()), float(y.mean())
    cx = ((x + xn) * cross).sum() / (6.0 * area)
    cy = ((y + yn) * cross).sum() / (6.0 * area)
    return float(cx), float(cy)


def _static_points(angles: np.ndarray, ranges: np.ndarray, range_max: float) -> np.ndarray:
    r = np.asarray(ranges, dtype=np.float64)
    ok = np.isfinite(r) & (r > 0.05) & (r < 0.98 * range_max)
    a = np.asarray(angles, dtype=np.float64)[ok]
    rr = r[ok]
    return np.stack([rr * np.cos(a), rr * np.sin(a)], axis=1)


def _polar_profile(pts: np.ndarray, centre: tuple[float, float], bins: int) -> np.ndarray:
    """Median range per angular sector, seen from ``centre``; NaN where no point falls."""
    prof = np.full(bins, math.nan)
    if len(pts) == 0:
        return prof
    d = pts - np.asarray(centre)
    ang = np.arctan2(d[:, 1], d[:, 0])
    rng = np.hypot(d[:, 0], d[:, 1])
    idx = np.floor((ang + math.pi) / (2 * math.pi) * bins).astype(int) % bins
    for b in range(bins):
        sel = rng[idx == b]
        if sel.size:
            prof[b] = float(np.median(sel))
    # Fill isolated empty bins from their neighbours (re-centring thins some sectors).
    for b in np.flatnonzero(np.isnan(prof)):
        left, right = prof[(b - 1) % bins], prof[(b + 1) % bins]
        if not math.isnan(left) and not math.isnan(right):
            prof[b] = 0.5 * (left + right)
    return prof


def _segment_lengths(
    pts: np.ndarray,
    gap: float = 0.30,
    corner_deg: float = 40.0,
    stride: int = 4,
    min_len: float = 0.35,
    max_segments: int = MAX_SEGMENTS,
) -> list[float]:
    """Lengths of straight wall runs: split at range jumps > ``gap`` and at corners.

    Corners are detected on directions taken over ``stride`` points so that 1 cm range
    noise on 3-4 cm point spacing does not read as a corner.
    """
    n = len(pts)
    if n < 2 * stride + 2:
        return []
    cos_corner = math.cos(math.radians(corner_deg))
    lengths: list[float] = []
    cur = 0.0
    for k in range(1, n + 1):  # +1 closes the loop
        p0, p1 = pts[k - 1], pts[k % n]
        dist = float(np.hypot(*(p1 - p0)))
        if dist > gap:
            if cur >= min_len:
                lengths.append(cur)
            cur = 0.0
            continue
        back = pts[k - 1] - pts[(k - 1 - stride) % n]
        fwd = pts[(k - 1 + stride) % n] - pts[k - 1]
        nb, nf = float(np.hypot(*back)), float(np.hypot(*fwd))
        if nb > 1e-9 and nf > 1e-9 and nb <= stride * gap and nf <= stride * gap:
            if float(back @ fwd) / (nb * nf) < cos_corner:
                if cur >= min_len:
                    lengths.append(cur)
                cur = 0.0
        cur += dist
    if cur >= min_len:
        lengths.append(cur)
    lengths.sort(reverse=True)
    return [float(v) for v in lengths[:max_segments]]


# --- public API ------------------------------------------------------------------------------


def fingerprint_from_static(
    angles: np.ndarray, static_ranges: np.ndarray, range_max: float, *, note: str = ""
) -> RoomFingerprint:
    """Build the fingerprint from the lidar background (per-beam static ranges)."""
    pts = _static_points(angles, static_ranges, range_max)
    centre = _polygon_centroid(pts)
    prof = _polar_profile(pts, centre, PROFILE_BINS)
    segs = _segment_lengths(pts)
    return RoomFingerprint(
        profile=[float(v) for v in prof], segments=segs, centroid=centre, note=note
    )


def _profile_similarity(
    a: np.ndarray, b: np.ndarray, tol_soft: float, tol_hard: float
) -> float:
    both_open = np.isnan(a) & np.isnan(b)
    one_open = np.isnan(a) ^ np.isnan(b)
    ok = ~(both_open | one_open)
    score = np.zeros(len(a))
    score[both_open] = 1.0
    diff = np.abs(a[ok] - b[ok])
    score[ok] = np.clip((tol_hard - diff) / (tol_hard - tol_soft), 0.0, 1.0)
    return float(score.mean()) if len(a) else 0.0


def _segments_similarity(a: list[float], b: list[float]) -> float:
    k = min(len(a), len(b))
    if k == 0:
        return 0.0 if (a or b) else 1.0
    aa, bb = np.asarray(a[:k]), np.asarray(b[:k])
    rel = np.abs(aa - bb) / np.maximum(aa, bb)
    # Missing/extra segments on either side count against the match.
    penalty = abs(len(a) - len(b)) / max(len(a), len(b))
    return float(max(0.0, (1.0 - rel.mean()) * (1.0 - 0.5 * penalty)))


def room_match(
    fp: RoomFingerprint,
    angles: np.ndarray,
    static_ranges: np.ndarray,
    range_max: float,
    *,
    tol_soft: float = 0.10,
    tol_hard: float = 0.35,
    profile_weight: float = 0.90,
) -> float:
    """Similarity in [0, 1] of the current static scan to ``fp`` (rotation-tolerant)."""
    cur = fingerprint_from_static(angles, static_ranges, range_max)
    ref = np.asarray(fp.profile, dtype=np.float64)
    now = np.asarray(cur.profile, dtype=np.float64)
    if len(ref) != len(now):
        return 0.0
    best = 0.0
    for rot in range(len(ref)):
        s = _profile_similarity(ref, np.roll(now, rot), tol_soft, tol_hard)
        if s > best:
            best = s
    seg = _segments_similarity(fp.segments, cur.segments)
    return float(min(1.0, max(0.0, profile_weight * best + (1.0 - profile_weight) * seg)))


def save_fingerprint(fp: RoomFingerprint, path: Path) -> None:
    path = path.expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(fp.to_json())
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def load_fingerprint(path: Path) -> RoomFingerprint | None:
    path = path.expanduser()
    if not path.exists():
        return None
    return RoomFingerprint.from_json(path.read_text())


class RoomMatcher:
    """Holds the enrolled fingerprint (if any) and scores static scans against it."""

    def __init__(self, path: Path) -> None:
        self.path = path.expanduser()
        self.fingerprint: RoomFingerprint | None = load_fingerprint(self.path)
        self.last_match: float | None = None

    @property
    def enrolled(self) -> bool:
        return self.fingerprint is not None

    def enroll(self, angles: np.ndarray, static_ranges: np.ndarray, range_max: float) -> None:
        fp = fingerprint_from_static(angles, static_ranges, range_max, note="pincer enroll_room")
        save_fingerprint(fp, self.path)
        self.fingerprint = fp
        self.last_match = 1.0

    def match(
        self, angles: np.ndarray, static_ranges: np.ndarray, range_max: float
    ) -> float | None:
        if self.fingerprint is None:
            return None
        self.last_match = room_match(self.fingerprint, angles, static_ranges, range_max)
        return self.last_match
