# Demo on the first chassis

Everything Pincer1 does, on the test chassis, with the laptop beside it: the robot stands and looks at
you, the daemon reads the transaction out through the robot's speaker and on its front panel, you
confirm on the robot, the signature goes back to the dapp. The signing hardware is the software signer
on the laptop for now; the consent pad is a double tap on the body.

## once

```
cd robot && uv venv -p 3.12 .venv-demo && uv pip install -p .venv-demo/bin/python -e ".[vision,voice]" elevenlabs sounddevice "setuptools<81"
cp .env.example .env     # then: PINCER_BODY_BACKEND=joint, PINCER_BODY_PROFILE=chassis, PINCER_ROSBRIDGE_URL, PINCER_CAMERA_URL,
                         # PINCER_AUDIO_SINK=ssh:pi@<robot>, ELEVENLABS_API_KEY / VOICE_ID
.venv-demo/bin/python -m pincer_robot.enroll --no-voice      # your face from the robot's camera, passphrase, duress word
cd ../daemon && cp .env.example .env && pnpm i                 # CONTROLLER=sim, RPC_URL devnet for a rehearsal
cd ../extension && pnpm i && pnpm build                        # chrome://extensions, load dist/ unpacked
```

The robot: its own stack running (rosbridge on 9090, the camera stream on 8080), nothing else driving
the servos. The chassis calibration ships in the code (`PulseCalibration.chassis`); `pincer-calibrate
--preset chassis` confirms the directions on a new unit.

## every time

```
cd daemon && pnpm dev                                    # 1. the wallet
cd robot && .venv-demo/bin/python -m pincer_robot.main   # 2. the robot: stands, raises its head, starts watching
cd extension/demo && python3 -m http.server 8765         # 3. a dapp: http://localhost:8765
curl -XPOST localhost:7401/pair/new                      # 4. the code the extension popup asks for
```

Devnet SOL for the wallet address the daemon prints at start: faucet.solana.com.

## walking

On this chassis the legs walk with its own gait controller (`PINCER_CHASSIS_GAIT=chassis`, the
default): the body package drives the head and the arm, the chassis keeps its stance and walks the
way it already knows how to, so the "turn to the owner and step closer" of the attention move is
there. `PINCER_CHASSIS_GAIT=own` puts the body package's gait on the joints instead; try it on a stand
first.

## the run

1. Stand in front of the robot. The head turns to your face; the front panel says `watching`.
2. In the dapp: Connect, then Send. The daemon parses the transfer, the robot turns to you and says
   what it is; the panel shows the amount and the destination.
3. Double-tap the robot's body. The signature returns to the dapp, the explorer link appears.
4. Leave the room (or cover the camera): the daemon locks; a Send now is refused before anything is
   signed. Say the duress word instead of tapping: the decoy answers.

What is not on this chassis: the touch pad and the trusted display (the panel is the chassis' own OLED,
driven by the main computer), the secure element (software signer). Say so.
