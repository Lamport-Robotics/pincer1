# Pincer1 locator beacon

A small board hidden inside the robot's body with its own 18650 cell. It keeps working when the main
computer is off or the robot is carried away, and when triggered it reports where the robot is over
two channels that need no internet on the robot:

- **Apple Find My** (BLE). The beacon advertises as a lost tag (the OpenHaystack scheme). Any iPhone
  that passes within Bluetooth range relays an encrypted location report to Apple; you fetch and
  decrypt it with the private keys only you hold.
- **LoRa** (SX1262, 868 / 915 / 923 MHz) to a base station at home, a few kilometres in the open,
  less in a city. The base prints what it hears on USB; `relay.py` turns that into ntfy / Telegram /
  webhook notifications and sends authenticated commands back.

Cellular is deliberately not in v1.

The beacon reveals **location**, never funds. Nothing in it can sign: the key safety story in
`docs/SECURITY.md` is unchanged. It is off until you provision and arm it.

## Honest limits

- Find My needs an iPhone to pass within ~10–30 m of the robot at some point after the alarm, with
  Bluetooth on. In a flat, a car or a warehouse that may be minutes or never. Reports reach Apple
  only when that iPhone has internet, and the fetcher sees them with a delay of minutes to hours.
- LoRa needs the base in range. At the settings used here (SF11, 125 kHz) that is roughly 2–5 km
  with line of sight and a few hundred metres through buildings. Once the robot is out of range
  the base hears nothing until it comes back.
- Neither is instant. The alarm packet goes out at once with the last known position; a fresh GNSS
  fix takes up to 90 s outdoors and may never come indoors (then the last known position is sent,
  with its age).
- A thief who finds the beacon can remove its cell. It is hidden, not armoured.
- The 18650 cell lasts weeks armed (deep sleep, a 20 s IMU sample, an hourly packet) and roughly a
  day or two in alarm (GNSS, BLE advertising and LoRa receive all on). Keep the cell charged from
  the robot's 5 V rail through the board's USB connector; the beacon then only draws on its own cell
  when the robot is off.

## What is in here

```
beacon/
  platformio.ini         envs: tbeam-supreme, tbeam-v12 (beacon), base-heltec (base), native (tests)
  src/
    main.cpp             state machine glue: deep sleep, wake handling, alarm loop
    triggers.{h,cpp}     PURE: debounce + arm/alarm transitions, grace window, cadences   (host-tested)
    packet.{h,cpp}       PURE: 48-byte LoRa packet, ChaCha20-Poly1305, replay protection  (host-tested)
    aead.{h,cpp}         PURE: ChaCha20-Poly1305, RFC 8439, no dependencies                (host-tested)
    sha256.{h,cpp}       PURE                                                              (host-tested)
    findmy.{h,cpp}       PURE: key derivation (X9.63 KDF, P-224 scalars), advertisement    (host-tested)
    findmy_p224.cpp      scalar → public key with mbedTLS (ESP32 core; host only if mbedTLS is present)
    findmy_ble.{h,cpp}   NimBLE advertising with the random static address
    lora.{h,cpp}         SX1262 through RadioLib, region table, send / timed receive
    gps.{h,cpp}          TinyGPSPlus on the UART, rail gated through the PMU
    board.{h,cpp}        pins, AXP2101 rails, QMI8658 IMU, battery, deep sleep (two boards)
    config.h             tunables and the baked-in provisioning
    base_main.cpp        shim that compiles base/src/main.cpp for env:base-heltec
  test/                  Unity tests for the pure modules (`pio test -e native`)
  tools/
    provision.py         device id + LoRa key + Find My seed → beacon.env (gitignored)
    findmy_keys.py       the Find My key list for the fetcher, from the seed (pure Python)
    keyflags.py, pio_keys.py, host_mbedtls.py   build plumbing
  base/
    src/main.cpp         base station firmware: receive, verify, print JSON; send commands
    relay.py             JSON lines → ntfy / Telegram / webhook; commands back (HTTP, CLI, Telegram)
    relay.service        systemd unit
```

## Hardware

| part | role |
|---|---|
| LilyGO **T-Beam Supreme** (ESP32-S3, SX1262, u-blox GNSS, QMI8658 IMU, AXP2101 PMU, 18650 holder) | the beacon |
| LilyGO T-Beam v1.2 (ESP32, SX1262, AXP2101, no IMU) | also builds (`tbeam-v12`): no motion trigger |
| 18650 Li-ion cell, protected or with the PMU's own protection | the beacon's battery |
| normally-closed micro switch, on the body shell | tamper |
| Heltec **WiFi LoRa 32 V3** (ESP32-S3, SX1262) on USB at home | the base station (`base-heltec`); a second T-Beam works with the same code |
| LoRa antennas for both, matched to the region | **never transmit without one** |

The T-Beam's own USB-C powers the board and charges the cell: wire it to the robot's 5 V buck through
a short cable. With the robot off, the beacon runs on its cell.

### Wiring (T-Beam Supreme)

| T-Beam Supreme | signal | from | notes |
|---|---|---|---|
| GPIO 14 | **TRIG_CTRL** | consent controller **GP16** | 3.3 V, active high; high for 2 s = duress, tamper or wipe. Trusted. |
| GPIO 15 | **TRIG_ROBOT** | a Jetson 3.3 V GPIO (e.g. header pin 7, GPIO 09) | active high: robot lifted with no owner in sight. Untrusted; worst case a false alarm. |
| GPIO 16 | **TAMPER** | NC switch to GND | the pin is pulled up; opening the shell (or cutting the wire) pulls it high |
| GND | ground | both boards | common ground with the controller and the Jetson |
| USB-C | 5 V | robot 5 V rail | power + charging |

Add a 100 kΩ pull-down from each TRIG line to GND and a 100 kΩ pull-up on TAMPER near the beacon:
the ESP32's internal pulls are kept alive in deep sleep, but external ones do not depend on it.
The Jetson's GPIOs are 3.3 V; the Pico 2's are 3.3 V. Neither needs a level shifter.

All three inputs are RTC-capable GPIOs so they can wake the ESP32-S3 from deep sleep. If your header
exposes different ones, change `PIN_TRIG_*` in `src/board.h` (RTC GPIOs are 0–21 on the S3). The
T-Beam v1.2 defaults are GPIO 13 / 14 / 25.

A line that stays high (a Jetson holding TRIG_ROBOT up, a cut tamper wire) is acted on once; while it
stays high it is left out of the wake mask and re-checked every minute, so it cannot keep the beacon awake.

The IMU interrupt on the Supreme (GPIO 33) is not an RTC pin, so motion is **polled**: while armed the
beacon wakes every 20 s, reads the accelerometer and compares the gravity vector with the previous
sample kept in RTC memory.

Mount the beacon where the shell hides it, antenna away from the aluminium plates (a flexible 868/915
antenna taped inside a plastic part works; metal all around means no signal), and the tamper switch
where the shell's fasteners are.

### Placement of the trigger sources

- **TRIG_CTRL** is driven by the consent controller firmware (`PIN_BEACON`, GP16): a 2 s pulse on
  duress, tamper (`usb_reset`, brownout) and the PIN wipe. This is the trusted input: in `idle` it is
  the only one that counts.
- **TRIG_ROBOT** is driven by `pincer_robot` on the Jetson when `moved` is true with no owner in
  sight. It is a convenience, not a guarantee: a compromised main computer can raise it (a false
  alarm) or never raise it (the other inputs cover that).

## Build, provision, flash

PlatformIO (`pip install platformio`, or the VS Code extension). From `beacon/`:

```bash
pio test -e native

python3 tools/provision.py --region AS923      # Thailand; EU868 / US915 otherwise → beacon.env
pio run -e tbeam-supreme -t upload             # the beacon (hold BOOT while plugging in if needed)
pio run -e base-heltec -t upload               # the base station
pio device monitor -b 115200                   # watch either

python3 tools/findmy_keys.py --env beacon.env --count 2000 > findmy_keys.json   # for the fetcher
```

`beacon.env` holds the device id, the 32-byte LoRa key and the 32-byte Find My seed, mode 0600,
gitignored. The build bakes them in through `tools/pio_keys.py` (set `BEACON_ENV=/path/file` to use a
file elsewhere; `tools/keyflags.py` prints the equivalent `-D` flags). Without the file the firmware
builds **unprovisioned** and refuses to arm.

Both the beacon and the base must be built from the **same** `beacon.env`.

### Keys in flash, stated plainly

The LoRa key and the Find My seed sit in the ESP32's flash in the clear (no flash encryption in v1).
Anyone who opens the robot, finds the beacon and dumps it has them. What they get: the ability to read
the robot's position reports and to send it `disarm`. What they do not get: anything that moves funds,
or any key of the consent controller or the SE050. That trade is accepted and is why the beacon is a
separate board with separate secrets. Keep `beacon.env` off the robot: it is the only copy of the
Find My seed, and the seed is the only way to decrypt the reports.

## Running it

**States.** `idle` → `armed` → `alarm`.

- Arm and disarm from the base: `python3 base/relay.py send arm` (or `disarm`, `ping`, `ok`), a
  `POST /cmd` to the relay, or `/arm` in your Telegram chat when `TELEGRAM_COMMANDS=1`. The base
  repeats the command every 2 s until the beacon acknowledges (up to 20 min: an idle or armed beacon
  listens for 3 s every 15 min; in alarm it listens all the time).
- On the bench: hold the T-Beam's user button for 3 s to toggle idle/armed (never out of alarm), or
  type `arm` / `disarm` / `status` / `test-alarm` on the USB serial.
- `ok` tells the beacon the owner is moving the robot: the soft triggers (robot line, motion) are
  ignored for 30 min. Tamper and the controller line are never ignored.

**Triggers** (`src/triggers.h` has the rules and the numbers):

| input | counts in | debounce | notes |
|---|---|---|---|
| TRIG_CTRL | idle, armed | 2 s high | duress / tamper / wipe from the consent controller |
| TAMPER | armed | 200 ms | shell opened |
| TRIG_ROBOT | armed | 500 ms | soft: suppressed for 30 min after `ok` |
| motion (IMU) | armed | 5 min of sustained movement (pauses under 90 s do not end an episode) | soft, same grace |
| heartbeat | armed | every hour | battery + coarse (0.01°) position |

**Alarm.** GNSS on; the first LoRa alarm packet goes out at once with the last known position, the next
one when a fix arrives or after 90 s; then every 2 min for the first hour and every 10 min after that
(the receiver only runs before each packet after the first hour, to spare the cell). Find My
advertising runs continuously with a new key every 15 min; the key index is in every packet and kept
in flash. The radio listens between packets. The alarm ends on an authenticated `disarm` (→ idle) or
`arm` (→ armed, keeps watching), or when the cell dies. Later triggers add their bit to the cause.

## Base station and relay

Flash `base-heltec`, plug it into any always-on machine (a Raspberry Pi, the home server, not the
robot), and run the relay:

```bash
pip install pyserial
RELAY_PORT=/dev/ttyACM0 NTFY_URL=https://ntfy.sh/<private-topic> python3 base/relay.py
```

Environment, all optional: `NTFY_URL` (+ `NTFY_TOKEN`), `TELEGRAM_BOT_TOKEN` + `TELEGRAM_CHAT_ID`
(+ `TELEGRAM_COMMANDS=1` to accept `/arm /disarm /ping /ok /status` from that chat only),
`WEBHOOK_URL`, `RELAY_HTTP` (default `127.0.0.1:7410`), `RELAY_SILENT_AFTER_S` (notify once when an
armed beacon's heartbeats stop; default 2 h), `RELAY_ALARM_EVERY_S` (re-notify an ongoing alarm at most
this often, sooner if it moved more than 200 m). Alarms go out as `urgent`; everything else is logged
and sent at normal priority. `base/relay.service` is a systemd unit; put the variables in
`/etc/pincer-relay.env`.

The base prints one JSON object per line (`base/src/main.cpp` documents them), so anything else can
consume it too. It keeps its own counters in flash and drops replays; the relay drops them again from
its state file.

The relay needs internet to reach you. The beacon and the base never do.

## Find My

Key derivation is documented in `src/findmy.h` and mirrored by `tools/findmy_keys.py`:

```
d0   = X963KDF-SHA256(seed, "pincer1-findmy-d0")  mod (n−1) + 1
SK_0 = X963KDF-SHA256(seed, "pincer1-findmy-sk0")
SK_i = X963KDF-SHA256(SK_{i−1}, "update")
(u_i, v_i) = X963KDF-SHA256(SK_i, "diversify", 72 bytes), each mod (n−1) + 1
d_i  = (d0·u_i + v_i) mod n          P_i = d_i·G on P-224, advertised key = x(P_i)
```

The rotation (SK/u/v/d_i) is the scheme of Apple's accessory protocol as published by Heinrich et al.
(PETS 2021) and used by OpenHaystack and FindMy.py; the two lines that derive `d0` and `SK_0` from one
seed are ours (OpenHaystack draws them at random). Honest status: **this derivation has been verified
for determinism between the C code and the Python helper, and the Python helper against the
`cryptography` package for the KDF and the curve arithmetic. It has not been verified against an Apple
device or a live fetcher.** For the fetcher that does not matter: it takes a list of private keys and
asks Apple for reports under SHA-256 of each public key, which is what `findmy_keys.py` produces, in
OpenHaystack's export format (`privateKey` + `additionalKeys`, which macless-haystack imports) or as
JSONL/CSV for FindMy.py. Generate a window that covers the alarm duration you care about (2000 keys =
20 days of continuous alarm; the beacon only advances the index while advertising, and tells the base
the current index in every packet).

The advertisement is the 31-byte Apple manufacturer frame (`4C 00 12 19 <status> <22 key bytes> <2 bits> 00`)
on a random static address built from the first six key bytes, non-connectable, every second. The
status byte carries a battery level in its top two bits as reverse-engineered from AirTags; nothing
else in it is relied on.

To read reports you need an Apple ID logged in on a Mac (OpenHaystack) or the
[macless-haystack](https://github.com/dchristl/macless-haystack) /
[FindMy.py](https://github.com/malmeloo/FindMy.py) route. Those projects carry their own caveats about
Apple's terms and anisette servers; this repository does not include a fetcher.

## Privacy

Armed, the beacon sends a heartbeat every hour with a **coarse** (0.01°, about 1 km) position of the
robot over LoRa to your base; that is your own location when the robot lives with you. Set
`BEACON_HEARTBEAT_POSITION=0` in `beacon.env` to send heartbeats with no position at all. Find My
advertising only runs in alarm; nothing is advertised while idle or armed. The beacon is idle after
flashing until you arm it. The relay sends nothing anywhere unless you set a destination.

## Security notes

- Every LoRa packet (both directions) is ChaCha20-Poly1305 under the shared key with a nonce of
  device id ‖ seq; the direction bit in the seq keeps the two nonce spaces apart. Receivers accept
  only increasing seqs and persist them, so a recorded `disarm` cannot be replayed. The beacon
  reserves counter blocks in flash before use, so a reboot never reuses a nonce.
- A jammer can stop LoRa. Find My is independent of it.
- Anyone with a BLE sniffer sees that *something* advertises like a lost AirTag; the key rotates every
  15 min so they cannot follow it across rotations without the seed.
- The board button and the USB serial can arm/disarm when the beacon is idle or armed but never end an
  alarm. Both are inside the shell: reaching them means opening it, which trips the tamper switch.

