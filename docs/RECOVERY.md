# Recovery

What to do when something is lost: the words, the robot, the PIN, your freedom for an afternoon, or you.
Read this once while everything still works, and keep a copy with the words.

## What exists, and where

| thing | where it lives | what it recovers |
|---|---|---|
| 24 real words | paper, written from the robot's display at setup; never on the Jetson, never on the wire | the real wallet, on any device, forever |
| 24 decoy words | same paper or a second one | the decoy wallet (it holds real, small funds) |
| PIN rhythm | your hands and the SE050 (5 attempts, then wipe) | nothing: it cannot be changed or read back; a new PIN is a new wallet |
| duress rhythm | your hands and the SE050 | nothing; replaceable any time under the real PIN |
| inheritance arrangement | the controller's own flash region, signed once | the heir's transaction, released by the controller when the timer runs out |
| pairing tokens, settings, agent history, enrolment | `/var/lib/pincer1` on the Jetson | convenience only; all of it can be recreated |
| firmware signing key, release key, Jetson PKC/SBK | offline, with the words; never on the robot | the ability to ship updates to provisioned boards |

The controller is the wallet. The Jetson, the daemon, the browser extension and the robot body are
replaceable parts around it. Any Solana wallet that accepts a 24-word phrase on `m/44'/501'/0'/0'`
restores the real account; EVM uses `m/44'/60'/0'/0/0`, Bitcoin `m/84'/0'/0'/0/0` (native segwit).

## Lost the words, the robot still works

Nothing is lost yet, but nothing is backed up either: the words cannot be shown again (`reveal_seed` is
refused by design). Make a new wallet and move the funds while the old one still signs:

1. Create the new wallet elsewhere first (a second Pincer1, or any wallet whose words you have written
   down). Fund it with a dust amount and check you can spend from it.
2. Send everything from the robot to the new wallet, through the robot as usual: it reads the transfer
   aloud, you touch. Tokens first, then SOL (keep rent for the last transaction).
3. If an inheritance arrangement is armed, clear it (`POST /inheritance/clear`, long touch), otherwise it
   stays valid and spends from an account you have emptied; that is harmless but confusing for the heir.
4. Wipe and re-initialise, or keep using the old key for small amounts. To wipe deliberately: tap a wrong
   rhythm five times at `unlock`. The controller zeroes its flash and deletes the key and PIN objects; the
   next daemon start runs `init` again and shows new words. Write them down this time.

## Lost, dead or stolen robot

Import the real words into any wallet and you are back. Then decide whether the key is still yours:

- **Dead or lost** (nobody has it): keep using the words, or move to a new seed at leisure.
- **Stolen**: assume the thief will try. The SE050 gives them five rhythm attempts, the controller boots
  locked, a modified firmware does not boot, and extracting the key from the chip is a lab job, not an
  evening's work. Still, move the funds to a fresh seed now, from the words, and treat the old key as
  burnt. The decoy wallet, if it held anything, moves the same way with the decoy words.
- The **inheritance arrangement** on a stolen controller cannot be cancelled by the thief (clearing needs
  the real PIN and the original key); it can only be released when the inactivity timer expires, and it
  spends whatever is then in the old account. If you emptied the account, the release is worth nothing.
  If you want the arrangement to survive the theft, leave the funds and let it run; the heir gets them.
- The **beacon**, if fitted, keeps reporting on its own battery over Find My and LoRa.

A new robot is set up from scratch (`docs/SETUP.md`). Do not import the old words into it: a fresh key on
a fresh controller, then move the funds across with the old words from a laptop wallet.

## Five wrong rhythms

The fifth wrong rhythm at `unlock` answers `denied:wiped`: flash is zeroed, the key and PIN objects are
deleted in the chip, the controller is uninitialised. This is the designed outcome whether the fingers
were a thief's or yours after a bad night. The words restore the wallet elsewhere; the robot re-initialises
with a new key the next time the daemon starts. The inheritance region survives the wipe (see above). The
daily spend counters, routers and policy are gone; set them again.

## After duress

You tapped the duress rhythm or said the word; the decoy opened; they took what the decoy had; they left.
The real wallet refused everything meanwhile and nothing on the robot showed it existed. Afterwards:

1. Tap the real rhythm at the next `unlock`. The real wallet is back.
2. Move the real funds to a new seed anyway if the attackers handled the robot or watched you tap: the
   rhythm has little entropy and a watched tap is a known tap.
3. Re-fund the decoy; an empty decoy convinces nobody next time. `GET /duress/decoy` shows its addresses.
4. Replace the duress rhythm (`POST /duress/set_pin`) if it was seen.
5. If the duress *word* was used, the daemon sent the silent alert and the beacon pulsed; check where
   those went and who was told.

## When the owner is gone (heirs)

What the heir needs is in two places, and either one is enough:

- **The words**, if the owner left them (a sealed envelope, a lawyer, a split). Import them anywhere.
- **The arrangement on the controller**, if the owner armed the dead-man's switch. The controller counts
  *powered* time since the last touch, so the robot must stay plugged in and untouched for the whole
  inactivity period (`inactivity_days`, typically 90). A touch resets it, including a well-meaning one;
  tell the household. When it expires:
  1. Connect to the daemon on the robot (`http://<robot>:7401`, same LAN) or plug the controller into
     any computer and talk to it over USB (`docs/PROTOCOL.md` §1; `inheritance_status` says `armed`,
     `inactivity_days`, `days_since_touch`).
  2. `POST /inheritance/release` (or `inheritance_release` over USB) returns the signed transaction. It
     works while the controller is locked, wiped or in duress; no PIN is needed.
  3. Broadcast it with any Solana RPC (`POST /inheritance/release` already does when the daemon is
     online). The amount is what the owner signed at arming time; anything above it stays in the
     account, reachable only with the words.
  The transaction depends on a durable nonce account that must not have been advanced since arming. If
  the owner re-signed as balances changed, the newest arrangement is the one stored; older ones are void.

Nothing in the robot can produce the words after the fact, and no support channel can either.

## Firmware

- **An update that did not take**: the controller boots the new image on trial; if the daemon never
  confirms it (`update_commit`), three boots later the image erases itself and the previous one runs.
  Power-cycle until `hello.fw` shows the old version, then try again with a good build. Keys, PIN, policy
  and the inheritance region are outside both partitions; an update never touches them.
- **Lost the firmware signing key**: provisioned boards (`provision-otp.sh`) will accept no further
  firmware, ever. The wallet is unaffected; the boards simply stay on their last image. Keep the key with
  the words.
- **Leaked signing key**: anyone can sign for provisioned boards. Treat every unit as a fresh target:
  move the funds via the words, retire the boards, provision new ones with a new key.
- **Lost the release key** (Jetson software): make a new one, install the new public key on each robot
  by hand (`/etc/pincer1/release.pub`), continue.

## Jetson

Anything on the Jetson is replaceable: reflash JetPack, run `docs/SETUP.md` §2 to §4, plug the same
controller in. The wallet is there with its PIN; pair the browser again. If you kept a copy of
`/var/lib/pincer1` (settings, enrolment, agent history), restore it before starting the services; if not,
enrol again. A release that breaks the daemon rolls back on its own (`deploy/jetson/update`); to go back by
hand, `sudo pincer-rollback`.
