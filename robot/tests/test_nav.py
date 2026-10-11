import asyncio
from pathlib import Path

import pytest

from pincer_robot.nav import (
    FindResult,
    Localization,
    MockNav,
    Navigator,
    RoomRegistry,
    room_match_from_localization,
)


def _registry(tmp_path: Path) -> RoomRegistry:
    reg = RoomRegistry(tmp_path / "rooms.json")
    reg.add("kitchen", 8.0, 0.0)
    reg.add("living", 4.0, 0.0, radius=2.5)
    reg.add("bedroom", 0.0, 6.0)
    reg.add("desk", 0.0, 2.0, radius=1.0)
    reg.add("hall", 0.0, 0.0, polygon=[(-1, -1), (1, -1), (1, 1), (-1, 1)])
    return reg


def test_registry_point_in_radius_and_polygon(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    assert reg.room_at(0.5, 0.5) == "hall"  # polygon wins
    assert reg.room_at(0.0, 2.5) == "desk"
    assert reg.room_at(0.0, 7.5) == "bedroom"
    assert reg.room_at(5.0, 0.0) == "living"
    assert reg.room_at(6.3, 0.0) == "kitchen"  # inside both radii: nearest centre (1.7 < 2.3)
    assert reg.room_at(5.9, 0.0) == "living"  # 1.9 < 2.1
    assert reg.room_at(20.0, 20.0) is None
    assert reg.get("Bedroom ") is not None
    assert reg.remove("hall") and reg.room_at(0.5, 0.5) is None


def test_registry_persists_and_validates(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    path = tmp_path / "rooms.json"
    assert oct(path.stat().st_mode & 0o777) == "0o600"
    again = RoomRegistry(path)
    assert again.names() == ["kitchen", "living", "bedroom", "desk", "hall"]
    assert again.rooms["hall"].polygon == [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]
    with pytest.raises(ValueError):
        reg.add("  ", 0, 0)
    with pytest.raises(ValueError):
        reg.add("tri", 0, 0, polygon=[(0, 0), (1, 1)])
    bad = RoomRegistry(tmp_path / "corrupt.json")
    (tmp_path / "corrupt.json").write_text("{not json")
    bad.load()
    assert bad.names() == []


def test_ordered_visits_last_seen_then_priority_then_rest(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    prio = ["bedroom", "desk", "living"]
    assert reg.ordered(prio) == ["bedroom", "desk", "living", "kitchen", "hall"]
    assert reg.ordered(prio, "kitchen") == ["kitchen", "bedroom", "desk", "living", "hall"]
    assert reg.ordered(prio, "nowhere") == ["bedroom", "desk", "living", "kitchen", "hall"]
    assert reg.ordered(["attic"], None) == ["kitchen", "living", "bedroom", "desk", "hall"]


def test_room_match_from_localization(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    assert room_match_from_localization(None, reg) == 0.0
    assert room_match_from_localization(Localization(0, 6, 0, cov_xy=0.05), reg) == 1.0
    assert room_match_from_localization(Localization(0, 6, 0, cov_xy=0.9), reg) == 0.0  # no fix
    assert room_match_from_localization(Localization(0, 6, 0, 0.05, score=0.2), reg) == 0.0
    assert room_match_from_localization(Localization(50, 50, 0, 0.05, in_map=True), reg) == 1.0
    assert room_match_from_localization(Localization(50, 50, 0, 0.05, in_map=False), reg) == 0.0


def _navigator(tmp_path: Path, nav: MockNav) -> Navigator:
    return Navigator(nav, _registry(tmp_path), tmp_path / "maps", dwell_s=1.0, goal_timeout_s=5.0)


def test_mapping_save_localize_and_name_room(tmp_path: Path) -> None:
    nav = MockNav(has_map=False)
    n = _navigator(tmp_path, nav)
    assert not n.available and n.localize() is None and n.room_match() is None
    with pytest.raises(RuntimeError):
        n.name_room("study")
    n.start_mapping()
    assert n.status()["mapping"] is True
    assert n.save_map("home") == tmp_path / "maps" / "home"
    assert (tmp_path / "maps" / "home.yaml").exists()
    assert n.available and not n.status()["mapping"]
    nav.pose = (0.0, 6.2, 0.3)
    assert n.localize() == (0.0, 6.2, 0.3, "bedroom")
    assert n.room_match() == 1.0
    nav.cov_xy = 2.0
    assert n.room_match() == 0.0
    nav.cov_xy = 0.01
    nav.pose = (3.0, -5.0, 0.0)
    r = n.name_room("study", radius=1.5)
    assert r.x == 3.0 and r.radius == 1.5 and n.current_room() == "study"
    assert "study" in n.status()["rooms"]


def test_goto_results(tmp_path: Path) -> None:
    nav = MockNav(has_map=False)
    n = _navigator(tmp_path, nav)
    res = asyncio.run(n.goto("bedroom"))
    assert not res.ok and res.reason == "no_map"
    nav.has_map = True
    res = asyncio.run(n.goto("attic"))
    assert not res.ok and res.reason == "unknown_room"
    res = asyncio.run(n.goto("bedroom"))
    assert res.ok and res.room == "bedroom" and nav.pose[:2] == (0.0, 6.0)
    nav.blocked.add((8.0, 0.0))
    res = asyncio.run(n.goto("kitchen"))
    assert not res.ok and res.reason == "blocked"


def _run_find(n: Navigator, visible_rooms: set[str], **kw) -> tuple[FindResult, list[str]]:  # noqa: ANN003
    checks: list[str] = []

    def visible() -> bool:
        room = n.current_room()
        checks.append(room or "-")
        return room in visible_rooms

    async def noop_sleep(_s: float) -> None:
        await asyncio.sleep(0)

    clock = {"t": 0.0}

    def tick() -> float:
        clock["t"] += 0.25
        return clock["t"]

    looked: list[int] = []

    async def look_around() -> None:
        looked.append(1)
        await asyncio.sleep(0)

    res = asyncio.run(
        n.find_owner(visible, look_around=look_around, clock=tick, sleep=noop_sleep, **kw)
    )
    return res, checks


def test_find_owner_goes_to_last_seen_room_first(tmp_path: Path) -> None:
    nav = MockNav(has_map=True, pose=(8.0, 0.0, 0.0))  # starts in the kitchen
    n = _navigator(tmp_path, nav)
    res, _ = _run_find(n, {"living"}, last_seen_room="living")
    assert res.found and res.room == "living" and res.travelled
    assert res.visited == ["living"]
    assert nav.goals[0][:2] == (4.0, 0.0)


def test_find_owner_priority_then_rest_and_skips_blocked(tmp_path: Path) -> None:
    nav = MockNav(has_map=True, pose=(8.0, 0.0, 0.0))
    nav.blocked.add((0.0, 2.0))  # desk unreachable
    n = _navigator(tmp_path, nav)
    res, _ = _run_find(n, {"hall"})
    assert res.found and res.room == "hall"
    assert res.visited == ["bedroom", "desk", "living", "kitchen", "hall"]


def test_find_owner_not_found_and_timeout_budget(tmp_path: Path) -> None:
    nav = MockNav(has_map=True)
    n = _navigator(tmp_path, nav)
    res, _ = _run_find(n, set())
    assert not res.found and res.travelled and res.room is None
    assert len(res.visited) == 5
    nav2 = MockNav(has_map=True)
    n2 = _navigator(tmp_path, nav2)
    res, _ = _run_find(n2, set(), max_secs=3.0)  # dwell 1 s + clock ticks: not every room
    assert not res.found and len(res.visited) < 5


def test_find_owner_returns_immediately_when_visible_or_without_map(tmp_path: Path) -> None:
    nav = MockNav(has_map=True, pose=(0.0, 6.0, 0.0))
    n = _navigator(tmp_path, nav)
    res, _ = _run_find(n, {"bedroom"})
    assert res.found and res.room == "bedroom" and not res.travelled and nav.goals == []
    nav.has_map = False
    res, _ = _run_find(n, set())
    assert not res.found and not res.travelled and nav.goals == []
