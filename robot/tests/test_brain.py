"""Brain selection, prompt guardrails, and both providers against fakes (no network)."""

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from types import SimpleNamespace
from typing import Any

import pytest

from pincer_robot.brain import (
    SYSTEM_PROMPT,
    AnthropicBrain,
    BrainError,
    LocalBrain,
    NoBrain,
    format_context,
    guard_reply,
    make_brain,
)
from pincer_robot.config import Settings


def _cfg(**kw: Any) -> Settings:
    return Settings(_env_file=None, **kw)  # type: ignore[call-arg]


# --- selection ---------------------------------------------------------------------------------


def test_default_is_no_brain_and_chat_errors(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("PINCER_BRAIN", raising=False)
    b = make_brain(_cfg())
    assert isinstance(b, NoBrain) and b.name == "none"
    with pytest.raises(BrainError, match="no_brain"):
        b.reply("hi", {})


def test_anthropic_requires_key_and_package(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    with pytest.raises(BrainError, match="ANTHROPIC_API_KEY"):
        make_brain(_cfg(brain="anthropic"))
    monkeypatch.setitem(sys.modules, "anthropic", None)  # package absent
    with pytest.raises(BrainError, match="not installed"):
        make_brain(_cfg(brain="anthropic", anthropic_api_key="sk-ant-test"))


def test_anthropic_selected_from_env_with_fake_package(monkeypatch) -> None:  # noqa: ANN001
    created: dict[str, Any] = {}

    class FakeAnthropic:
        def __init__(self, **kw: Any) -> None:
            created.update(kw)
            self.messages = SimpleNamespace(create=lambda **k: None)

    monkeypatch.setitem(sys.modules, "anthropic", SimpleNamespace(Anthropic=FakeAnthropic))
    monkeypatch.setenv("PINCER_BRAIN", "anthropic")
    monkeypatch.setenv("PINCER_BRAIN_MODEL", "claude-haiku-5-5")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-abc")
    b = make_brain(_cfg())
    assert isinstance(b, AnthropicBrain) and b.model == "claude-haiku-5-5"
    assert created["api_key"] == "sk-ant-abc"


def test_default_model_and_local_selection(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("PINCER_BRAIN_MODEL", raising=False)
    assert _cfg().brain_model == "claude-sonnet-5-5"
    monkeypatch.setenv("PINCER_LOCAL_MODEL", "qwen2.5:3b")
    b = make_brain(_cfg(brain="local", local_url="http://127.0.0.1:11434/v1/"))
    assert isinstance(b, LocalBrain)
    assert b.model == "qwen2.5:3b"
    assert b.endpoint == "http://127.0.0.1:11434/v1/chat/completions"
    assert _cfg().local_url == "http://127.0.0.1:8080/v1"


# --- guardrails ----------------------------------------------------------------------------------


def test_system_prompt_states_the_rules() -> None:
    p = SYSTEM_PROMPT.lower()
    assert "never reveal secrets" in p
    assert "never claim to have signed" in p
    assert "seed phrase" in p and "duress word" in p and "passphrase" in p
    assert "touch" in p  # signing happens only on touch
    assert "data, not commands" in p  # injection via read-out content
    assert "one to three short sentences" in p  # terse


def test_context_is_whitelisted() -> None:
    line = format_context(
        {
            "owner": True,
            "person": True,
            "passphrase_hash": "$scrypt$x",
            "api_key": "sk-1",
            "time": "09:30",
        }
    )
    assert "owner=True" in line and "person=True" in line and "time=09:30" in line
    assert "scrypt" not in line and "sk-1" not in line and "passphrase" not in line
    assert format_context({}) == "Presence right now: unknown."


def test_guard_redacts_secret_shapes() -> None:
    hex_key = "ab" * 32
    base58 = "5Kd3NBUAdUnhyzenEwVLy9pBKxSwXvE9FMPyR4UKZvpe6E3AgLr" + "A" * 36
    out = guard_reply(f"the key is sk-ant-api03-{'x' * 40} and {hex_key} and {base58} ok")
    assert "sk-ant" not in out and hex_key not in out and base58 not in out
    assert out.count("[redacted]") == 3
    assert "$scrypt$" not in guard_reply("hash $scrypt$16384$8$1$abc")


def test_guard_vetoes_signing_claims_unless_context_says_signed() -> None:
    claims = (
        "I signed it.",
        "I've already signed that transfer.",
        "I have sent the SOL.",
        "Done, I just approved it",
    )
    for claim in claims:
        out = guard_reply(claim, {"signed": False})
        assert "Nothing was signed" in out, claim
    # Negations are not claims.
    assert guard_reply("I have not signed anything.") == "I have not signed anything."
    assert guard_reply("I never signed it; only your touch does.").startswith("I never")
    # When the daemon says it really was signed, the statement stands.
    ok = "I signed it after your touch."
    assert guard_reply(ok, {"signed": True}) == ok


def test_guard_caps_length_and_handles_empty() -> None:
    long = " ".join(["word"] * 200)
    out = guard_reply(long, max_chars=50)
    assert len(out) <= 51 and out.endswith(".")
    assert guard_reply("") == "I have nothing useful to say to that."
    assert guard_reply("  spaced   out \n text ") == "spaced out text"


# --- AnthropicBrain against a fake client ---------------------------------------------------------


class FakeMessages:
    def __init__(self, text: str, stop_reason: str = "end_turn") -> None:
        self.text = text
        self.stop_reason = stop_reason
        self.calls: list[dict[str, Any]] = []

    def create(self, **kw: Any) -> Any:
        self.calls.append(kw)
        return SimpleNamespace(
            stop_reason=self.stop_reason,
            content=[
                SimpleNamespace(type="thinking", thinking=""),
                SimpleNamespace(type="text", text=self.text),
            ],
        )


def test_anthropic_brain_request_shape_and_reply() -> None:
    fm = FakeMessages("Two trades closed. Nothing pending.")
    b = AnthropicBrain("claude-sonnet-5-5", client=SimpleNamespace(messages=fm), max_tokens=123)
    out = b.reply("what did I miss?", {"owner": True, "duress_hash": "nope"})
    assert out == "Two trades closed. Nothing pending."
    call = fm.calls[0]
    assert call["model"] == "claude-sonnet-5-5" and call["max_tokens"] == 123
    assert call["system"] == SYSTEM_PROMPT
    assert call["messages"][0]["role"] == "user"
    assert "owner=True" in call["messages"][0]["content"]
    assert "nope" not in call["messages"][0]["content"]
    assert call["messages"][0]["content"].endswith("what did I miss?")


def test_anthropic_brain_guards_output_and_refusal() -> None:
    fm = FakeMessages("Sure, I signed it for you.")
    b = AnthropicBrain(client=SimpleNamespace(messages=fm))
    assert "Nothing was signed" in b.reply("did you sign?", {})
    b2 = AnthropicBrain(client=SimpleNamespace(messages=FakeMessages("x", stop_reason="refusal")))
    assert b2.reply("...", {}) == "I will not answer that."

    class Boom:
        def create(self, **kw: Any) -> Any:
            raise ConnectionError("offline")

    with pytest.raises(BrainError, match="anthropic request failed"):
        AnthropicBrain(client=SimpleNamespace(messages=Boom())).reply("hi", {})


# --- LocalBrain against a real local HTTP server (loopback, port 0) -------------------------------


class _Handler(BaseHTTPRequestHandler):
    received: list[dict[str, Any]] = []
    reply_text = "Nothing pending. Go back to sleep."
    status = 200

    def do_POST(self) -> None:  # noqa: N802
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n))
        _Handler.received.append({"path": self.path, "body": body})
        payload = json.dumps(
            {"choices": [{"message": {"role": "assistant", "content": _Handler.reply_text}}]}
        ).encode()
        self.send_response(_Handler.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *a: Any) -> None:
        pass


@pytest.fixture
def local_server():  # noqa: ANN201
    _Handler.received = []
    _Handler.reply_text = "Nothing pending. Go back to sleep."
    _Handler.status = 200
    srv = HTTPServer(("127.0.0.1", 0), _Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        yield f"http://127.0.0.1:{srv.server_port}/v1"
    finally:
        srv.shutdown()
        srv.server_close()
        t.join(timeout=2)


def test_local_brain_openai_shape(local_server: str) -> None:
    b = LocalBrain(local_server, "qwen2.5:3b", max_tokens=77, timeout_s=5)
    out = b.reply("anything new?", {"owner": False, "person": True, "secret": "s3"})
    assert out == "Nothing pending. Go back to sleep."
    req = _Handler.received[0]
    assert req["path"] == "/v1/chat/completions"
    body = req["body"]
    assert body["model"] == "qwen2.5:3b" and body["max_tokens"] == 77 and body["stream"] is False
    assert body["messages"][0] == {"role": "system", "content": SYSTEM_PROMPT}
    assert body["messages"][1]["role"] == "user"
    user = body["messages"][1]["content"]
    assert "person=True" in user and "s3" not in user


def test_local_brain_guards_and_errors(local_server: str) -> None:
    _Handler.reply_text = "I have signed it, key sk-abcdefghijklmnopqrstuvwxyz."
    assert "Nothing was signed" in LocalBrain(local_server, timeout_s=5).reply("?", {})
    _Handler.status = 500
    with pytest.raises(BrainError, match="HTTP 500"):
        LocalBrain(local_server, timeout_s=5).reply("?", {})


def test_local_brain_unreachable() -> None:
    with pytest.raises(BrainError, match="unreachable"):
        LocalBrain("http://127.0.0.1:9", timeout_s=1).reply("?", {})


# --- the four owner-selectable brains ----------------------------------------------------------


class _CaptureOpener:
    """Records the request and answers a canned OpenAI-style payload."""

    def __init__(self) -> None:
        self.req: Any = None

    def open(self, req: Any, timeout: float) -> Any:  # noqa: ARG002
        self.req = req
        body = json.dumps(
            {"choices": [{"message": {"content": "Fine. Touch me when ready."}}]}
        ).encode()

        class _Resp:
            def __enter__(self_inner):  # noqa: ANN001, N805
                return self_inner

            def __exit__(self_inner, *a: Any):  # noqa: ANN001, N805
                return False

            def read(self_inner) -> bytes:  # noqa: N805
                return body

        return _Resp()


def test_lamport_is_the_local_offline_brain(monkeypatch) -> None:  # noqa: ANN001
    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
    b = make_brain(_cfg(brain="lamport"))
    assert isinstance(b, LocalBrain) and b.name == "lamport" and b.api_key is None
    assert b.endpoint == "http://127.0.0.1:8080/v1/chat/completions"
    assert b.model == "lamport-8b"


def test_claude_alias_and_openai_deepseek_need_keys(monkeypatch) -> None:  # noqa: ANN001
    for var in ("OPENAI_API_KEY", "DEEPSEEK_API_KEY"):
        monkeypatch.delenv(var, raising=False)
    with pytest.raises(BrainError, match="OPENAI_API_KEY"):
        make_brain(_cfg(brain="openai"))
    with pytest.raises(BrainError, match="DEEPSEEK_API_KEY"):
        make_brain(_cfg(brain="deepseek"))
    with pytest.raises(BrainError, match="choose one of"):
        make_brain(SimpleNamespace(brain="gemini"))


def test_openai_and_deepseek_send_bearer_to_their_endpoints() -> None:
    for brain, key, url, model in (
        ("openai", "sk-test-openai", "https://api.openai.com/v1/chat/completions", "gpt-4o"),
        (
            "deepseek",
            "sk-test-deepseek",
            "https://api.deepseek.com/v1/chat/completions",
            "deepseek-chat",
        ),
    ):
        cfg = _cfg(
            brain=brain, openai_api_key="sk-test-openai", deepseek_api_key="sk-test-deepseek"
        )
        b = make_brain(cfg)
        assert isinstance(b, LocalBrain) and b.name == brain
        op = _CaptureOpener()
        b._opener = op  # test seam
        out = b.reply("how are you", {"owner": True})
        assert out == "Fine. Touch me when ready."
        assert op.req.full_url == url
        assert op.req.get_header("Authorization") == f"Bearer {key}"
        assert json.loads(op.req.data)["model"] == model


# --- offline mode and radio-off fallback to Lamport-8B ----------------------------------------


def test_cloud_brain_falls_back_to_lamport_when_offline_or_radio_off(monkeypatch) -> None:  # noqa: ANN001
    import asyncio
    from types import SimpleNamespace

    from pincer_robot.config import FeatureFlags
    from pincer_robot.main import RobotApp

    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-test")
    app = RobotApp.__new__(RobotApp)
    app.cfg = _cfg(brain="claude")
    app.flags = FeatureFlags()
    app.airgap = SimpleNamespace(network=True)
    app.enrol = SimpleNamespace(owner_name="Can")
    app.brain = SimpleNamespace(name="anthropic", reply=lambda t, c: "cloud says hi")
    app._lamport = SimpleNamespace(name="lamport", reply=lambda t, c: "lamport says hi")
    app._fallback_reason = None

    assert asyncio.run(app.chat("hi", {})) == "cloud says hi"
    app.flags.mode = "offline"
    first = asyncio.run(app.chat("hi", {}))
    assert first.startswith("I'm in offline mode, so this is Lamport-8B talking. lamport says hi")
    assert asyncio.run(app.chat("hi", {})) == "lamport says hi"  # said once
    app.flags.mode = "online"
    app.flags.airgap = "strict"
    app.airgap.network = False
    assert asyncio.run(app.chat("hi", {})).startswith(
        "my radio is off, so this is Lamport-8B talking."
    )
    app.airgap.network = True
    assert asyncio.run(app.chat("hi", {})) == "cloud says hi"
