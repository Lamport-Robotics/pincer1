# Setup: from a bare robot to a paired browser

## 1. Build and flash the controller

See `firmware/README.md`. Generate the signing key once, provision the Pico 2's OTP with its hash (one-way),
then flash the signed `.uf2` by holding BOOTSEL while plugging the board in. On first boot the LED blinks
slowly: no key yet.

## 2. Prepare the Jetson (once per board)

1. Flash JetPack 6.x with NVIDIA SDK Manager from an Ubuntu host, rootfs on the NVMe.
2. **Secure boot and disk encryption.** Generate the PKC signing key (and optionally an SBK) on an offline
   machine, then burn the fuses with NVIDIA's `odmfuse.sh` from the Linux_for_Tegra tools and re-flash with
   the signed, encrypted images (`l4t_initrd_flash.sh` with the rootfs-encryption config). Follow the
   "Secure Boot" and "Disk Encryption" chapters of the Jetson Linux Developer Guide for your exact JetPack
   version; the flags change between releases, so we do not copy them here. **Fuses are one-way.** A board
   fused to a key only boots images signed with it, forever. Keep the keys with the Pico 2 signing key,
   offline.
3. `sudo nvpmodel -m 0 && sudo jetson_clocks` for full performance; ROS2 Humble via apt on 22.04.
4. Copy the udev rule: `sudo install -m 644 deploy/jetson/udev/99-pincer1.rules /etc/udev/rules.d/ && sudo udevadm control --reload`.

## 3. Install the robot software (Jetson)

```bash
deploy/jetson/robot/install.sh          # apt deps, venv with the [jetson] extra, systemd unit
pincer-enroll                           # face (10 frames), voice + passphrase, duress word
sudo systemctl start pincer-robot       # presence service on :7402, ROS2 /scan and /imu, camera, mic
```

Laptop mock mode and the Raspberry Pi fallback are in `robot/README.md`.

## 4. Install and start the daemon (Jetson)

```bash
deploy/jetson/daemon/install.sh         # node 22, build, /opt/pincer1/daemon, systemd unit pincerd
sudoedit /etc/pincer1/daemon.env        # RPC_URL, ADDRESS_BOOK, ORIGIN_ALLOWLIST; CONTROLLER=/dev/pincer-consent
sudo systemctl restart pincerd && journalctl -u pincerd -f
```

On the first run the daemon sees `initialized:false` and asks the controller to create a key. Hold your hand
on the robot's head for three seconds, then tap your PIN rhythm (4–8 taps, short and long gaps) twice. The
PIN is verified inside the SE050; five wrong rhythms wipe the device. The 24 words appear **only on the
robot's display**, four per page, the real set and then the decoy set; tap through and write both down. The
daemon never sees them. It does print the decoy wallet's public addresses once: **fund the decoy** with a
small, believable balance, because an empty decoy convinces nobody. `GET /duress/decoy` shows the addresses
again after your PIN.

The daemon then offers a **duress rhythm**: a second tap code, different from your PIN, tapped twice. From
then on, tapping it where you would tap your PIN opens the decoy wallet, and nothing on the robot, the
screen or the wire shows the difference. Skip with a 3-second hold; set it later with
`POST /duress/set_pin`. The public addresses are printed on every start.

## 5. Pair the browser

```bash
curl -XPOST localhost:7401/pair/new     # the robot speaks a 6-digit code (valid 2 minutes)
```

Load `extension/dist` as an unpacked extension, open the popup, enter the robot's address
(`ws://pincer1.local:7401` or its LAN IP) and the code. The robot says "Paired."

Any Solana dapp now lists **Pincer1** as a wallet. Pick it, and every request is read out by the robot and
signed on touch.

## 6. Daily use

- **Approve**: listen, then touch the head. A long touch (3 s) is needed for amounts over the daily limit,
  for policy changes, and for granting the overnight allowance.
- **Leave the room**: the robot locks. Come back, let it see you, tap your PIN rhythm to unlock.
- **Duress**: say the duress word, or tap the duress rhythm instead of your PIN. The decoy wallet opens, the
  real one locks. The word also sends a silent alert; the rhythm sends nothing, since you may be standing
  next to the person forcing you.
- **Night**: `curl -XPOST localhost:7401/agent/arm` (or ask the robot) → long touch → it works the budget.
  In the morning it reads the report and asks about proposals one at a time.
- **Morning**: turn on the briefing once and pick what it covers:

  ```bash
  curl -XPOST localhost:7401/settings -d '{"briefing":{"enabled":true,"time":"07:30","coins":["bitcoin","solana","hyperliquid"],"topics":["solana","etf","hack"]}}'
  ```

  The first time it sees you after 07:30 it reads prices, the headlines that match your topics, your
  balance and anything the house needs you to know. `GET /briefing` previews it; `POST /briefing/now` reads
  it on demand.

## 7. Recovery

Lost or dead robot: import the 24 words into any Solana wallet (standard `m/44'/501'/0'/0'`). The decoy
wallet has its own 24 words shown at setup; keep those too, it holds real (small) funds.
