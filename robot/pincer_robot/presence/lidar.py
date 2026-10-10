"""Human-in-room detection from a 2D LaserScan.

Pipeline per frame:

1. **Background model.** For the first ``learn_seconds`` the detector records, per
   beam, the *farthest* valid return it has seen. Walls, cupboards, table legs end
   up in the background; a person walking through during learning does not, because
   the beam also sees the wall behind them at some point. After learning, beams that
   keep returning at or beyond the background slowly track it (drift), beams that
   return *closer* than ``background - margin`` are foreground.
2. **Clustering.** Foreground points are clustered by euclidean gap between
   consecutive foreground beams (wrapping around 360 degrees). A gap of 0.3 m is big
   enough to join two legs into one object.
3. **Person candidates.** Clusters 0.2-0.7 m wide at 0.3-4 m range.
4. **Motion.** Candidates are tracked between frames by nearest centroid. A track that
   has displaced more than ``move_threshold`` is "moving"; confidence is
   ``size_only_conf`` for a static person-sized object (a chair carried in after
   learning) and rises to 1.0 once it moves, decaying slowly if it stays still.

Returns a confidence in [0, 1] plus the bearing of the best candidate so the body
can turn toward the owner, and ``persons``: how many distinct person-sized tracks are
currently live with confidence >= ``person_count_conf`` (i.e. they have moved like a
person, not just been carried in). The two-person rule reads that count.

The learned background (``background``) doubles as the static map of the room that
``presence/room.py`` fingerprints.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

_TWO_PI = 2.0 * math.pi


@dataclass(frozen=True)
class Scan:
    """A sensor_msgs/LaserScan reduced to what we need (plain numpy, no ROS)."""

    angle_min: float
    angle_increment: float
    ranges: np.ndarray
    ts: float
    range_min: float = 0.05
    range_max: float = 12.0

    @property
    def angles(self) -> np.ndarray:
        n = len(self.ranges)
        return self.angle_min + self.angle_increment * np.arange(n, dtype=np.float64)

    def valid_mask(self) -> np.ndarray:
        r = np.asarray(self.ranges, dtype=np.float64)
        return np.isfinite(r) & (r >= self.range_min) & (r <= self.range_max)

    def to_xy(self) -> np.ndarray:
        r = np.asarray(self.ranges, dtype=np.float64)
        a = self.angles
        return np.stack([r * np.cos(a), r * np.sin(a)], axis=1)


@dataclass
class LidarConfig:
    learn_seconds: float = 30.0
    bg_margin: float = 0.20
    bg_drift_alpha: float = 0.02
    cluster_gap: float = 0.30
    min_points: int = 3
    person_width: tuple[float, float] = (0.20, 0.70)
    person_range: tuple[float, float] = (0.30, 4.0)
    move_threshold: float = 0.10
    move_window_s: float = 1.5
    still_hold_s: float = 20.0
    still_decay_s: float = 20.0
    size_only_conf: float = 0.30
    track_assoc_dist: float = 0.60
    track_timeout_s: float = 1.0
    person_count_conf: float = 0.5  # a track counts as a person above this confidence


@dataclass
class Cluster:
    points: np.ndarray
    centroid: np.ndarray
    width: float

    @property
    def range_m(self) -> float:
        return float(np.hypot(self.centroid[0], self.centroid[1]))

    @property
    def bearing(self) -> float:
        """Radians, 0 = straight ahead (+x), positive = left (+y), ROS convention."""
        return float(math.atan2(self.centroid[1], self.centroid[0]))


@dataclass
class Track:
    track_id: int
    pos: np.ndarray
    first_pos: np.ndarray
    first_ts: float
    last_ts: float
    width: float
    hits: int = 1
    moved_at: float | None = None
    history: list[tuple[float, np.ndarray]] = field(default_factory=list)

    @property
    def range_m(self) -> float:
        return float(np.hypot(self.pos[0], self.pos[1]))

    @property
    def bearing(self) -> float:
        return float(math.atan2(self.pos[1], self.pos[0]))


@dataclass
class LidarResult:
    confidence: float
    bearing: float | None
    range_m: float | None
    learning: bool
    ts: float
    clusters: list[Cluster] = field(default_factory=list)
    tracks: list[Track] = field(default_factory=list)
    persons: int = 0


def cluster_width(points: np.ndarray) -> float:
    """Largest distance between any two points (chord for very large clusters)."""
    n = len(points)
    if n < 2:
        return 0.0
    if n <= 400:
        d = points[:, None, :] - points[None, :, :]
        return float(np.sqrt((d * d).sum(axis=2)).max())
    return float(np.linalg.norm(points[0] - points[-1]))


def cluster_points(xy: np.ndarray, mask: np.ndarray, gap: float, min_points: int) -> list[Cluster]:
    """Group foreground points (``mask``) into clusters by euclidean gap, wrapping at 360 deg."""
    idx = np.flatnonzero(mask)
    if idx.size == 0:
        return []
    pts = xy[idx]
    groups: list[list[int]] = [[0]]
    for k in range(1, len(idx)):
        if np.linalg.norm(pts[k] - pts[k - 1]) <= gap:
            groups[-1].append(k)
        else:
            groups.append([k])
    if len(groups) > 1 and np.linalg.norm(pts[groups[0][0]] - pts[groups[-1][-1]]) <= gap:
        groups[0] = groups.pop() + groups[0]
    out: list[Cluster] = []
    for g in groups:
        if len(g) < min_points:
            continue
        p = pts[g]
        out.append(Cluster(points=p, centroid=p.mean(axis=0), width=cluster_width(p)))
    return out


class PersonDetector:
    """Stateful detector; feed it every scan in order and read the result."""

    def __init__(self, cfg: LidarConfig | None = None) -> None:
        self.cfg = cfg or LidarConfig()
        self._bg: np.ndarray | None = None
        self._learn_start: float | None = None
        self._tracks: list[Track] = []
        self._next_id = 1
        self.last_result: LidarResult | None = None
        self._last_scan: Scan | None = None

    # -- background ---------------------------------------------------------------
    @property
    def learning(self) -> bool:
        return self._bg is None or self._learn_start is None

    def reset_background(self) -> None:
        self._bg = None
        self._learn_start = None
        self._tracks.clear()

    @property
    def background(self) -> np.ndarray | None:
        """Per-beam static ranges (copy) once learned; ``None`` while learning."""
        if self.learning or self._bg is None:
            return None
        return self._bg.copy()

    def static_scan(self) -> tuple[np.ndarray, np.ndarray, float] | None:
        """(angles, static ranges, range_max) of the learned background, for room.py."""
        bg = self.background
        if bg is None or self._last_scan is None:
            return None
        return self._last_scan.angles, bg, self._last_scan.range_max

    def _update_background(self, scan: Scan, valid: np.ndarray, r: np.ndarray) -> bool:
        """Returns True while still in the learning phase."""
        n = len(r)
        if self._bg is None or len(self._bg) != n:
            self._bg = np.zeros(n, dtype=np.float64)
            self._learn_start = scan.ts
            self._tracks.clear()
        assert self._learn_start is not None
        bg = self._bg
        if scan.ts - self._learn_start < self.cfg.learn_seconds:
            bg[valid] = np.maximum(bg[valid], r[valid])
            return True
        # Beams that never returned during learning: nothing static within range.
        never = bg <= 0
        bg[never] = scan.range_max
        # Drift: farther returns extend the background immediately; near-background
        # returns track slowly. Foreground returns leave the model untouched.
        farther = valid & (r > bg)
        bg[farther] = r[farther]
        near = valid & (r <= bg) & (r >= bg - self.cfg.bg_margin)
        a = self.cfg.bg_drift_alpha
        bg[near] = (1.0 - a) * bg[near] + a * r[near]
        return False

    # -- tracking -----------------------------------------------------------------
    def _associate(self, candidates: list[Cluster], ts: float) -> None:
        cfg = self.cfg
        unmatched = list(candidates)
        for tr in sorted(self._tracks, key=lambda t: -t.hits):
            if not unmatched:
                break
            dists = [float(np.linalg.norm(c.centroid - tr.pos)) for c in unmatched]
            j = int(np.argmin(dists))
            if dists[j] <= cfg.track_assoc_dist:
                c = unmatched.pop(j)
                tr.history.append((tr.last_ts, tr.pos))
                tr.history = [(t, p) for t, p in tr.history if ts - t <= cfg.move_window_s]
                tr.pos = c.centroid
                tr.last_ts = ts
                tr.width = c.width
                tr.hits += 1
                disp_window = max(
                    (float(np.linalg.norm(c.centroid - p)) for _, p in tr.history), default=0.0
                )
                disp_origin = float(np.linalg.norm(c.centroid - tr.first_pos))
                if disp_window >= cfg.move_threshold or disp_origin >= 2 * cfg.move_threshold:
                    tr.moved_at = ts
        for c in unmatched:
            self._tracks.append(
                Track(
                    track_id=self._next_id,
                    pos=c.centroid,
                    first_pos=c.centroid.copy(),
                    first_ts=ts,
                    last_ts=ts,
                    width=c.width,
                )
            )
            self._next_id += 1
        self._tracks = [t for t in self._tracks if ts - t.last_ts <= cfg.track_timeout_s]

    def _track_confidence(self, tr: Track, ts: float) -> float:
        cfg = self.cfg
        if tr.moved_at is None:
            motion = 0.0
        else:
            since = ts - tr.moved_at
            if since <= cfg.still_hold_s:
                motion = 1.0
            else:
                motion = max(0.0, 1.0 - (since - cfg.still_hold_s) / cfg.still_decay_s)
        return cfg.size_only_conf + (1.0 - cfg.size_only_conf) * motion

    # -- public ---------------------------------------------------------------------
    def update(self, scan: Scan) -> LidarResult:
        cfg = self.cfg
        r = np.asarray(scan.ranges, dtype=np.float64)
        valid = scan.valid_mask()
        self._last_scan = scan
        learning = self._update_background(scan, valid, r)
        if learning:
            res = LidarResult(0.0, None, None, True, scan.ts)
            self.last_result = res
            return res
        assert self._bg is not None
        fg = valid & (r < self._bg - cfg.bg_margin)
        clusters = cluster_points(scan.to_xy(), fg, cfg.cluster_gap, cfg.min_points)
        wmin, wmax = cfg.person_width
        rmin, rmax = cfg.person_range
        candidates = [c for c in clusters if wmin <= c.width <= wmax and rmin <= c.range_m <= rmax]
        self._associate(candidates, scan.ts)
        live = [t for t in self._tracks if t.last_ts == scan.ts]
        best: Track | None = None
        best_conf = 0.0
        persons = 0
        for t in live:
            c = self._track_confidence(t, scan.ts)
            if c >= cfg.person_count_conf:
                persons += 1
            if c > best_conf:
                best, best_conf = t, c
        res = LidarResult(
            confidence=best_conf,
            bearing=best.bearing if best else None,
            range_m=best.range_m if best else None,
            learning=False,
            ts=scan.ts,
            clusters=clusters,
            tracks=list(self._tracks),
            persons=persons,
        )
        self.last_result = res
        return res


def laserscan_to_scan(msg: object, ts: float) -> Scan:
    """Convert a sensor_msgs.msg.LaserScan (duck-typed) into a ``Scan``."""
    return Scan(
        angle_min=float(msg.angle_min),  # type: ignore[attr-defined]
        angle_increment=float(msg.angle_increment),  # type: ignore[attr-defined]
        ranges=np.asarray(msg.ranges, dtype=np.float64),  # type: ignore[attr-defined]
        ts=ts,
        range_min=float(msg.range_min),  # type: ignore[attr-defined]
        range_max=float(msg.range_max),  # type: ignore[attr-defined]
    )
