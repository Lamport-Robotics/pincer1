import asyncio
import io

from pincer_robot.speech import (
    PrintSpeaker,
    clamp_urgency,
    elevenlabs_voice_settings,
    espeak_args,
)


def test_espeak_args_get_louder_and_faster_with_urgency() -> None:
    calm = espeak_args("en", 165, "hello", 0.0)
    loud = espeak_args("en", 165, "hello", 1.0)
    assert calm == ["-v", "en", "-s", "165", "-a", "100", "hello"]
    assert loud[loud.index("-a") + 1] == "200"
    assert int(loud[loud.index("-s") + 1]) == 231
    mid = espeak_args("en", 165, "x", 0.5)
    assert mid[mid.index("-a") + 1] == "150"
    assert espeak_args("en", 165, "hello", 7.0) == loud  # clamped


def test_elevenlabs_settings_lower_stability_as_urgency_rises() -> None:
    assert elevenlabs_voice_settings(0.0) is None
    a = elevenlabs_voice_settings(0.25)
    b = elevenlabs_voice_settings(1.0)
    assert a is not None and b is not None
    assert b["stability"] < a["stability"] < 0.5
    assert b["stability"] >= 0.1
    assert b["style"] > a["style"]
    assert clamp_urgency(-3) == 0.0 and clamp_urgency(2) == 1.0


def test_print_speaker_records_urgency() -> None:
    out = io.StringIO()
    sp = PrintSpeaker(stream=out)
    asyncio.run(sp.say("calm"))
    asyncio.run(sp.say("LOUD", urgency=0.8))
    assert sp.spoken == ["calm", "LOUD"]
    assert sp.urgencies == [0.0, 0.8]
    assert "(urgency 0.8) LOUD" in out.getvalue()
