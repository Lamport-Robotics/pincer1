# Bench

The consent controller without the robot: a Pico 2, the OLED, the touch pad and, if you have one, an SE050
breakout on a breadboard, talking to the daemon on a laptop. Everything the owner relies on when signing
(parse, display, touch, sign) is on this board; the hexapod around it is presence and convenience.
Build this first, then put it in a body.

## parts

| part | role |
|---|---|
| Raspberry Pi Pico 2 (RP2350), with headers | the controller |
| 2.42" 128×64 OLED, SSD1309, 4-wire SPI (7-pin: GND VCC SCK SDA RES DC CS) | the trusted display |
| TTP223 capacitive touch module | the touch pad |
| SE050 I2C breakout (SE050C or E; the Arduino-shield board works with jumpers) | the secure element. Optional on the bench: without it the software signer runs, `hello` reports `se050:false` and the daemon refuses `PINCER_MODE=prod` |
| 2 × 4.7 kΩ | I2C pull-ups, only if the breakout has none |
| half-size breadboard, jumper wire, micro-USB cable | |

Everything runs on the Pico 2's 3V3 pin. Nothing on this board touches 5 V or the servo rail.

## wiring

The same pins as the robot (`HARDWARE.md`, "Wiring"), so the board moves into the body unchanged:

| Pico 2 | goes to |
|---|---|
| GP4 / GP5 | SE050 SDA / SCL |
| GP15 | TTP223 OUT |
| GP17 GP18 GP19 | OLED CS, SCK (D0), SDA (D1) |
| GP20 / GP21 | OLED DC / RES |
| 3V3 / GND | OLED, TTP223 and SE050 power (SE050 ENA tied to 3V3) |
| GP26 | left floating (entropy) |
| USB | the laptop |

Keep the OLED leads short. Mount the TTP223 so the sensing side faces up and nothing conductive sits
under it; its onboard capacitor pads set the sensitivity, the default is right for a bare module.

## firmware

`firmware/README.md`, "Build". For a board without the chip:

```sh
cmake -S firmware -B build-soft -DPINCER_SOFT_SIGNER=ON -DPICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain/bin
cmake --build build-soft -j8
```

Hold BOOTSEL, plug in, copy `build-soft/pincer1_consent.uf2` to the drive that appears. With an SE050 use the
production build and run `firmware/scripts/provision-se050.md` once, which rotates the chip's platform keys
into the controller's OTP. The LED tells you the controller is alive: a heartbeat when idle, solid while the
pad is touched. The OLED shows `PINCER1 · NEW DEVICE` until the first run below creates the keys.

## daemon

```sh
cd daemon && pnpm i
CONTROLLER=/dev/tty.usbmodem101 ROBOT_WS=mock pnpm dev      # Linux: /dev/ttyACM0
curl -XPOST localhost:7401/pair/new
```

`ROBOT_WS=mock` stands in for the robot: presence is always "owner here", so the only gate left is the
board. On a new board the daemon sees `initialized:false` and asks the controller to create the keys: a long
touch on the pad, then the seed words page by page on the OLED, each shown twice (`SETUP.md`, first run).
Then load the extension, pair with the code, open a dapp and send a transaction. The request appears on the
OLED page by page; a touch after the last page returns the signature to the dapp, a three-second hold
refuses it. The PIN rhythm, the duress rhythm, daily limits and the allowlists all work on the bench, which
is where to practise them.

## what the bench is for

Firmware work: the parsers, the display pages, the touch and PIN timing, the SE050 channel. Anything you
change in `firmware/` is testable here with `curl` and the extension before it goes near the servos.

Not on the bench: presence (lidar, face, voice), the room lock, docking, the beacon. Those need the body.
