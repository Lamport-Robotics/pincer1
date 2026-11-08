"""The daemon's ``display`` readout on the chassis panel (PROTOCOL.md §2)."""

import asyncio

from pincer_robot.main import RobotApp


class Panel:
    def __init__(self) -> None:
        self.lines: list[tuple[str, str]] = []

    def show(self, title: str, body: str = "") -> None:
        self.lines.append((title, body))

    def beep(self, repeat: int = 1) -> None:
        pass


def _app() -> RobotApp:
    app = RobotApp.__new__(RobotApp)
    app.panel = Panel()
    app._panel_reset = None
    return app


def test_an_older_readouts_ttl_does_not_blank_a_newer_one() -> None:
    async def body() -> None:
        app = _app()
        await app.display("0.5 SOL", "to 9xQe", "low", 0.3)
        await asyncio.sleep(0.15)
        await app.display("2 SOL", "to 4kPz", "high", 1.0)  # the one the owner has to read now
        await asyncio.sleep(0.3)  # past the first readout's ttl
        assert app.panel.lines[-1] == ("2 SOL", "to 4kPz")
        await asyncio.sleep(0.9)  # past the second one's
        assert app.panel.lines[-1] == ("PINCER1", "watching")

    asyncio.run(body())


def test_a_readout_without_ttl_stays() -> None:
    async def body() -> None:
        app = _app()
        await app.display("0.5 SOL", "to 9xQe", "low", 0.2)
        await app.display("Locked", "", "low", 0)
        await asyncio.sleep(0.35)
        assert app.panel.lines[-1] == ("Locked", "")

    asyncio.run(body())
