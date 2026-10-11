# Hardware

## Platform

Pincer1 is its own machine. v1 is assembled from off-the-shelf parts around a six-legged chassis; v2 moves
to an open CAD body. Nothing here depends on a single vendor's kit; every part is listed so a unit can be rebuilt or repaired.

### Bill of materials (v1, one unit)

| subsystem | part | qty | why |
|---|---|---|---|
| main computer | NVIDIA Jetson Orin Nano Super Developer Kit, 8 GB | 1 | GPU for face/liveness/speech/local LLM; secure boot fuses; encrypted NVMe |
| storage | NVMe 2280 SSD, 256 GB (Samsung 980 / WD SN570 class) | 1 | encrypted rootfs |
| legs | Feetech STS3215 serial-bus servo (12 V, 30 kg·cm, magnetic encoder) | 18 | daisy-chained bus, position feedback, cheap to replace |
| arm | Feetech STS3215 | 4 | same bus and brackets as the legs; carries the camera head |
| servo bus driver | Waveshare Bus Servo Adapter (UART ↔ TTL bus) | 1 | one serial line from the Jetson to all 22 servos |
| chassis | 3 mm aluminium top/bottom plates + STS3215 bracket kit (CNC, open DXF in v2) | 1 | body, leg brackets, arm base |
| head camera | Luxonis OAK-D Lite (stereo depth + 4K RGB, USB-C, 61 g) | 1 | depth for liveness, light enough for the arm, JetPack 6 drivers |
| front display | Waveshare 4.3" HDMI IPS 800×480, capacitive touch | 1 | reads out amounts; HDMI + USB touch, no DSI needed on the dev kit |
| lidar | Slamtec RPLIDAR C1 (360°, 12 m, USB) | 1 | `/scan` for presence, room fingerprint, SLAM |
| IMU | Adafruit BNO085 (I2C) | 1 | tilt and "moved" detection |
| microphone | Seeed ReSpeaker USB Mic Array v2.0 | 1 | 4-mic beamforming, direction of voice |
| speaker | 2 × 3 W full-range + USB audio DAC/amp (Adafruit or similar) | 1 | loud enough to wake you |
| consent module | Raspberry Pi Pico 2 + NXP SE050 breakout + TTP223 + 2.42" SSD1309 OLED + 3.3 V relay module | 1 | see below |
| battery | 3S4P Li-ion 21700 pack (Molicel P42A / Samsung 50E), 11.1 V, 17–20 Ah ≈ 200 Wh, 30 A BMS, XT60 | 1 | ~4–5 h active, 10 h+ idle; servos and Jetson (9–20 V barrel) on one rail, no extra regulator |
| regulators | 5 V / 5 A buck for the consent module and USB devices | 1 | |
| misc | USB hub (powered), cabling, feet, fasteners | | |

Power budget: Jetson 10–15 W (25 W peak), 22 servos 20–25 W walking (80–100 W peaks), sensors + display +
consent module ≈ 9 W; about 40–50 W active and 20 W idle, so the 200 Wh pack gives roughly 4–5 h of
walking and talking and 10 h+ of standing watch. The 3S pack feeds the servo bus (11.1 V nominal, within
STS3215's range) and the Jetson's 9–20 V barrel input directly; a 5 V buck feeds the consent module, the
hub, the display and the lidar. The BMS low-voltage cutoff is set above the Jetson's 9 V floor.

### Locator beacon (optional)

| part | qty |
|---|---|
| LilyGO T-Beam Supreme (ESP32-S3, SX1262 LoRa, u-blox GNSS, IMU, PMU, 18650 holder) | 1 |
| 18650 Li-ion cell | 1 |
| normally-closed tamper switch on the body shell | 1 |
| base station: Heltec WiFi LoRa 32 V3 (or a second T-Beam) at home, on USB | 1 |

A board hidden in the body with its own battery and two radios, so it keeps working when the main computer
is off or the robot is carried away. Triggers: TRIG_CTRL from the consent controller (GP16: duress, tamper,
wipe), TRIG_ROBOT from a Jetson GPIO (robot lifted with no owner in sight), the tamper switch, and the
beacon's own motion sensor while armed. In alarm it reports position two ways that need no internet on the
robot: Apple's Find My network over BLE (any iPhone passing nearby relays the encrypted location to the
owner) and LoRa to the owner's base station within a few kilometres. Cellular is deliberately not in v1.
Software in `beacon/`; off until the owner provisions and enables it. Honest limits: Find My needs an
iPhone to pass within tens of metres at some point; LoRa needs the base in range; neither is instant.

### Charging dock (optional)

| part | qty |
|---|---|
| pogo-pin spring contacts, 2-pin, 10 A rated (Mill-Max / Adafruit), under the body | 1 pair |
| dock plate: 3 mm acrylic/aluminium with two copper strips 25 mm apart, against a wall | 1 |
| 3S Li-ion CC/CV charger brick, 12.6 V 3 A, barrel → strips through a Schottky diode (reverse polarity) | 1 |
| TI INA226 current/voltage monitor on the pack rail, I2C to the Jetson (2 mΩ shunt) | 1 |

The robot measures its own pack with the INA226 (voltage → state of charge, current sign → "charging").
With the home map and a registry entry named `dock` (recorded by putting the robot on the dock), it goes
to charge on its own below 20 % and leaves at 90 % or whenever it is needed (alarm, owner calls, a
signature request). The dock must stand against a wall: the final 0.5 m approach is lidar-guided on the
wall distance. Software in `robot/pincer_robot/{battery,dock}.py`; off until the owner enables it.

### Body (October 2026 design)

Hexapod base, a 4-axis arm on the top plate carrying the depth camera as the head, and a front-facing
display panel with the Lamport Robotics mark. The arm lets the camera look at the owner from a moving
viewpoint (parallax strengthens liveness) and look up at a standing person.

## The consent module

| part | role | notes |
|---|---|---|
| Raspberry Pi Pico 2 (RP2350) | consent controller | runs `firmware/` in Arm secure mode; signed firmware, boot key and SE050 channel keys in OTP; USB to the Pi 5 |
| NXP SE050 (OM-SE050ARD or any I2C breakout, SE050C/E variants work) | secure element | ed25519, CC EAL6+; key never leaves it |
| TTP223 capacitive touch module | the touch pad | mounted under the robot's head shell |
| 2.42" 128×64 monochrome OLED, SSD1309 controller, 4-wire SPI (7-pin module: GND VCC SCK SDA RES DC CS) | the trusted display | driven by the Pico 2 only; shows what is about to be signed, the PIN dots, the seed words at init. The 2.42" size is readable at arm's length through the shell window; the firmware's init sequence is for the SSD1309 |
| 2 × 4.7 kΩ resistors | I2C pull-ups | only if the breakout has none |
| jumper wire, 3.3 V | | |


## Wiring (Pico 2 pin numbers, identical to the Pico)

| Pico 2 | signal | goes to |
|---|---|---|
| GP4 | I2C0 SDA | SE050 SDA |
| GP5 | I2C0 SCL | SE050 SCL |
| 3V3 (pin 36) | power | SE050 VCC, SE050 ENA (tie high), TTP223 VCC |
| GND | ground | SE050 GND, TTP223 GND |
| GP15 | touch input | TTP223 OUT (active high) |
| GP17 | SPI0 CS | OLED CS |
| GP18 | SPI0 SCK | OLED SCK (SCL / D0) |
| GP19 | SPI0 MOSI | OLED SDA (D1) |
| GP20 | GPIO out | OLED DC |
| GP21 | GPIO out | OLED RES |
| 3V3 / GND | power | OLED VCC / GND (3.3 V logic and supply; the panel voltage comes from the module's own regulator) |
| GP16 | TRIG_CTRL to the beacon | high for 2 s on duress, tamper or wipe; the beacon's own battery keeps it alive when the robot is powered off |
| GP25 | on-board LED | feedback: heartbeat idle, slow blink touch, fast blink long touch, double blink PIN rhythm, solid while touched |
| GP26 | ADC0 | left floating: entropy source for the RNG |
| USB | CDC serial | Jetson USB port (`/dev/pincer-consent` via udev; `/dev/ttyACM0` without the rule) |

The touch pad and the OLED must be wired to the Pico 2 only. Never route either through the Jetson: the
whole security argument is that the main computer can neither fake a touch nor change what the owner reads
before touching. The same pad takes the PIN rhythm (4–8 taps). Keep the OLED's SPI leads short (under 15 cm)
and away from the servo rail; 8 MHz SPI over a flying lead is fine, a ribbon next to the servo bus is not.

## OTP: read before you touch it

The Pico 2 is provisioned once with `firmware/scripts/provision-otp.sh`: the hash of the firmware signing
key (secure boot), the three SE050 SCP03 keys, optionally debug disable. **OTP bits only go from 0 to 1;
nothing in this is reversible.** A board with secure boot enabled runs only firmware signed with that key
for the rest of its life; a board with the wrong SCP03 keys burned can never talk to its chip; a board with
debug disabled has no SWD. Provision on the bench, with the chip already rotated to the same keys
(`firmware/scripts/provision-se050.md`), and keep the signing key and key sets offline. A plain Pico
(RP2040) still builds for compile checks, but has none of this and is not a valid controller for v1.

## Placement

- The Pico 2 and SE050 sit inside the body shell, on the underside of the top plate, away from the servo
  power rail (noise). Short I2C leads, under 10 cm.
- The TTP223 pad goes under the top plate of the body, just behind the arm base, where a hand naturally
  rests when you lean over the robot; the plate is thin and the pad senses through it. A second pad under
  the display bezel is optional (same GPIO through a diode-OR, or GP14 as a second input). Sensitivity is
  set by the module's onboard capacitor pads; test through the actual shell.
- The trusted OLED goes behind a window in the head shell, next to the touch pad, facing the owner who
  leans over the robot: what it shows is the consent channel (`firmware/README.md`, "Trusted display"). It
  shows the seed words only during `init` and PIN feedback only as one dot per tap.
- The front (Jetson) display mirrors the amount and destination of the transaction being read out, so the
  owner reads and hears the same thing (daemon → robot `display` message); it is convenience, not consent,
  and never shows seed words.
- USB cable from the Pico 2 to the Jetson's USB-A, strain-relieved so a tug cannot unplug it (the controller
  reports `tamper:usb_reset` when it re-enumerates).

## Power

Pico 2 and SE050 together draw under 60 mA. Power them from the Jetson through the USB cable (the Pico 2
powers from VBUS). Do not share the servo battery rail directly.

## Why these parts

- **SE050 over ATECC608 / STSAFE / OPTIGA**: those do not support ed25519, which Solana requires. SE050
  does, and is the most widely deployed part with usable public documentation. Tropic Square's TROPIC01
  (open, auditable, ed25519) is the candidate for v2 once its tooling matures.
- **RP2350 (Pico 2) over driving the SE050 from the Jetson**: the consent logic must live on a device the
  main computer cannot alter at runtime. A small microcontroller with its own GPIO is the cheapest honest way to get that.
- **RP2350 over RP2040**: secure boot (signed images, key hash in OTP), OTP rows for the SE050 channel
  keys, a hardware TRNG, and the Cortex-M33 secure state. Same pins, same board outline, so v0 wiring
  carries over.

## v2 notes

- Remote attestation of the running firmware (the RP2350 can boot securely but cannot prove it to the
  daemon; a signed boot report from the SE050 would close that).
- An e-paper panel instead of the OLED for the trusted display (no burn-in on the idle screen, readable in
  sunlight); same SPI pins.
- TROPIC01 instead of SE050 for an open hardware story.
