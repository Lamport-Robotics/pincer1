<p align="center"><img src="brand/lockup-1600.png" alt="Lamport Robotics" width="520"></p>

# pincer1

A new kind of cold wallet. It's a robot.

Keys for Solana, EVM chains and Bitcoin live in an SE050 secure element inside a hexapod. A separate
microcontroller (RP2350, signed firmware) is the only thing that can talk to the chip, and it signs only
after re-parsing the transaction itself, showing it on its own display, and getting a physical touch. The
main computer (Jetson Orin Nano) runs presence detection (lidar, face, voice), the wallet daemon and the
companion brain, and is treated as compromised by design.

```
browser dapp ── Wallet Standard / EIP-6963 / PSBT ──▶ extension/
                                                        │ ws
                                                        ▼
                                                     daemon/   parse, explain, scam filter, presence gate
                                                      │     │ usb
                                              ws      │     ▼
                                                      ▼   firmware/   consent controller: own parser, display, touch, SE050
                                                   robot/   lidar / face / voice / speech / nav / dock
```

What's in here: `firmware/` (pico-sdk, C), `daemon/` (TypeScript), `robot/` (Python, ROS2 optional),
`extension/` (Chrome MV3), `beacon/` (ESP32 locator), `brain/` (Lamport-8B training pipeline, no weights),
`brand/`, `deploy/jetson/`, `docs/`.

The legs are in `robot/pincer_robot/body/`: leg kinematics, tripod / ripple / wave gaits, the serial
bus driver for the servos and a simulator the tests walk (`docs/BODY.md`). Not in here: the CAD. v1
bodies are built from the parts in `docs/HARDWARE.md`.

## features

Core: presence-gated signing, on-device transaction parsing for all three chains, trusted display, PIN as a
tap rhythm verified in the chip, duress wallet (word or second rhythm), scam filter, daily limits.

Optional, off by default: room lock, two-person rule, offline payments (durable nonce + QR), air-gap relay,
dead-man's switch, wake-up alarm, morning briefing, overnight agent with a touch-granted budget, home
mapping, charging dock, locator beacon, swaps through allowlisted routers. Brain: Lamport-8B on the robot
(offline), or Claude / ChatGPT / DeepSeek.

## run it on a laptop

```sh
cd daemon && pnpm i && ROBOT_WS=mock CONTROLLER=sim pnpm dev
curl -XPOST localhost:7401/pair/new
cd ../extension && pnpm i && pnpm build   # load dist/ unpacked, pair with the code
curl -XPOST localhost:7401/dev/touch -d '{}'   # the touch pad, in software
```

Each directory has its own README with build and test notes. Docs: `docs/SECURITY.md` (threat model),
`docs/PROTOCOL.md` (the three interfaces), `docs/HARDWARE.md`, `docs/SETUP.md`.

## license

MIT. Third-party code under `firmware/third_party` keeps its own licenses.
