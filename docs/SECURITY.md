# Security model

This document says exactly what Pincer1 protects, what it does not, and which component carries each
guarantee. Anything not listed here is not a claim we make.

## Trust boundaries

| component | trusted? | why |
|---|---|---|
| SE050 secure element | yes | keys are generated and used inside a CC EAL6+ chip; they cannot be read out |
| consent controller (RP2350 firmware) | yes | the only thing that can ask the SE050 to sign; it re-parses every transaction and requires a physical touch on a pin only it can read; secure boot means only our signed image runs, and only it holds the SE050 channel keys |
| daemon (`pincerd`, Jetson) | **no** | it runs network-facing code, the RPC client, the agent; assume it can be compromised |
| robot service (`pincer_robot`, Jetson) | **no** | same machine as the daemon |
| browser extension | **no** | untrusted input |
| the owner | final authority | the robot warns; it does not overrule a correctly warned owner |

The important consequence: **the presence checks (lidar, face, voice) run on an untrusted machine.** They
are a wall against remote attackers and social engineering, not the thing that keeps the key safe. The
key is kept safe by the controller + chip, which require a physical touch and an honest transaction.

## What the controller guarantees

1. **No touch, no signature.** The touch pad is wired to the controller, not to the Pi. A remote attacker
   with full control of the Pi can ask for a signature; nobody will touch the robot for them.
2. **What you approve is what the controller shows.** The controller has its own display, wired to it
   alone; the main computer cannot draw on it. Before it accepts a touch it renders its own parse of the
   request there: chain, amount, destination, any approval or unknown contract. Speech from the robot is a
   convenience layer and is not the consent channel. Solana messages, EVM transactions (ERC-20 transfers and
   approvals, router calls) and Bitcoin PSBTs (inputs, change, fee) are each parsed on the controller,
   including the destination. The daemon sends the bytes plus `claims`
   (SOL out, destination, programs, dangerous instructions). The controller parses the bytes itself and
   refuses when the claims understate what the bytes do. A compromised daemon can say "0.5 SOL" while
   sending 50; the controller says `claims_mismatch`.
3. **Dangerous instructions are never silent.** `setAuthority`, `approve`, `closeAccount`, `assign`,
   lookup-table writes must be declared or the controller refuses. The daemon's scam filter blocks them
   before they get that far; the controller is the backstop.
4. **Daily limit.** Enforced on the controller, persisted in its flash. The day rolls over only after at
   least 20 hours of the controller's own powered time and a host day change; host time that moves backwards
   or jumps forward is ignored, so the main computer cannot reset the counter. Raising it needs a 3-second
   touch.
5. **Allowance for the overnight agent** is bounded (programs, per-trade, cumulative, expiry), granted with
   a 3-second touch, and cleared by lock or duress.
6. **Lock** is only reversible with the PIN rhythm, verified inside the SE050. **Duress** switches to a
   decoy key slot and clears allowances. It has two triggers: the spoken duress word (robot → daemon →
   controller, with a silent alert) and a second tap rhythm verified inside the SE050 next to the real PIN.
   The rhythm path is silent and indistinguishable: the controller answers exactly as it does for a real
   unlock, counts no failure, and never reports to the daemon which wallet is active. The daemon cannot
   tell the decoy from the real wallet, so neither can anyone reading its screen.
7. **The recovery words never cross a wire.** At setup the 24 words (and the decoy's) are shown only on the
   controller's display and advanced by touch. The USB link carries public keys only; the daemon never sees
   or logs the words.
8. **Router allowlists live on the controller.** The routers that may be called for swaps are a built-in set
   plus owner additions made with the PIN; a list supplied by the main computer is informational only.
9. **`signMessage` cannot be a disguised transaction.** The controller refuses to sign a "message" that
   parses as a transaction message.
8. **Only our firmware runs on the controller.** The RP2350 boots only images signed with the Pincer1 key
   (hash in OTP). Reflashing the controller with hostile firmware does not get it to run.
9. **Only the controller can talk to the SE050.** Platform SCP03: every APDU is encrypted and MACed with
   keys stored in the controller's OTP and rotated into the chip at provisioning. A desoldered SE050, or
   the robot's Pi wired to the chip, gets no channel.
10. **The PIN is checked by the chip, not by us.** Signing keys carry an SE050 policy that permits signing
    only inside a session authenticated with the PIN object; the chip counts attempts (5) and locks the
    object. The firmware mirrors the counter and wipes storage and key objects on the fifth failure.
    After a wipe, the 24 words are the only way back.

## What the main computer adds (defence in depth, not key safety)

The Jetson Orin Nano is fused for secure boot (PKC) and runs an encrypted rootfs keyed to the SoC, so
someone with the robot cannot swap the SD/NVMe, edit the face or voice checks, or read enrolment data by
pulling the drive. Face, liveness, speech and the local language model run on the device: camera and
microphone data never leave the robot. None of this changes the trust table above: a compromised Jetson
still cannot sign, because it cannot touch the pad or talk to the SE050 without the controller.

## What the daemon and robot add (defence in depth, not key safety)

- Presence gate: the daemon does not even forward a request unless the robot reports a verified owner in
  the room within the last 6 s. In `prod` mode this cannot be disabled.
- Scam filter: drainer patterns, address poisoning against the address book, wallet-emptying sends,
  brand-new destinations, unknown and brand-new programs, durable-nonce tricks, untrusted origins.
- Spoken explanation of every request, with counterparty age and history, before the touch.
- Automatic lock when the owner leaves or the robot is moved.
- Duress word/expression → decoy wallet, silent alert.

## Threats

| threat | outcome |
|---|---|
| Remote compromise of the Jetson (daemon, robot service, OS) | No signature: nobody touches the robot. Attacker can read the public key, the address book, and the agent's history. |
| Malicious dapp / phishing site | Transaction is read aloud with intel; drainer patterns are blocked outright; the rest needs a conscious touch. |
| Compromised daemon lying about a transaction | Controller re-parses and refuses (`claims_mismatch` / `policy`). |
| Stolen seed phrase | Full loss. The seed is shown once at setup; store it offline and split it. Optional later: 2-of-3 with a phone key and a paper key. |
| Physical theft of the robot | Session locks on movement and the controller boots locked. Unlocking needs the PIN rhythm, verified in the SE050 with a 5-attempt limit, then wipe. Reflashing the controller is stopped by secure boot; moving the SE050 to another host is stopped by SCP03; extracting a key from the SE050 needs lab-grade attacks. The decoy slot under duress. With the optional beacon the robot also reports where it is, over Find My and LoRa, on its own battery. |
| Coercion ("sign this or else") | Duress word or duress rhythm opens the decoy wallet, which the owner funded at setup with a small, believable balance. The real wallet refuses everything until the real rhythm is tapped. |
| Owner approves a correctly warned bad transaction | Not prevented. Pincer1 makes the truth audible; it does not override the owner. |
| Replay of a recorded voice passphrase | Voice alone never authorises anything; face + lidar + touch are still required. |
| Modified controller firmware | Does not boot: RP2350 secure boot, key hash in OTP (one-way). Field updates land in the spare A/B partition, need the owner's long touch, boot on trial and roll back by themselves unless the daemon confirms the new image (`docs/PROTOCOL.md`, "update"). Remaining: nothing proves to the daemon *which* signed firmware is running (no attestation); a leaked signing key signs anything. |
| Fake robot (different hardware) | Not covered. The daemon pairs with whatever controller answers on USB; attestation is v2 work. |

## The optional features, and what each one trusts

| feature | enforced by | what it does not protect against |
|---|---|---|
| Room lock | robot service (untrusted) → daemon refuses | a compromised main computer can report a fake room; it still cannot touch the pad |
| Two-person rule | robot service → daemon refuses | same as above; it is a social control, not a cryptographic one |
| Offline payments | controller signs as usual | a signed durable-nonce tx is bearer-valid until sent: keep the QR like cash |
| Air-gap relay | robot service drives a relay | a compromised robot service can switch the radio on; the key is still behind the touch |
| Dead-man's switch | **controller**: own powered-time counter, touch = proof of life, signed tx held in its flash | the heir's transaction amount is fixed at signing (re-sign when balances drift); an attacker who keeps the robot powered and untouched for the full period triggers it, which is the designed behaviour |
| Wake-up alarm | daemon + robot | not a security feature; it can wake the wrong person if two people share the room |
| Locator beacon | its own board, battery and radios; triggered by the controller (duress, tamper, wipe), the robot (lifted), a case switch, or its own motion sensor | reveals the robot's location, never funds; the keys in its flash are readable with physical access to the beacon, which is accepted; reporting depends on an iPhone passing by or the base station being in range |

The dead-man's switch is the only optional feature with a hard guarantee, because it lives on the
controller. The others are conveniences and social controls layered on the same key safety as the core.

## Swaps

Swaps run only in online mode and only through allowlisted routers (Jupiter on Solana, 1inch v6 and 0x on
EVM, THORChain across chains). The daemon verifies the router address and that the quote's minimum output is
embedded in the transaction before asking for a touch; approvals are exact-amount and to the router only;
the controller treats any other destination as an unknown contract and refuses unless it was declared and
the owner touched. A compromised daemon can at most route a swap through a listed router at a worse price
within the slippage cap, never to an arbitrary address.

## Known gaps in v1

- The controller cannot resolve v0 address lookup tables on-device, so transactions that depend on them
  must declare `lookup_table` in claims and are treated as opaque; the daemon marks them `medium` risk and
  resolves the tables through the RPC for the spoken explanation.
- The seed phrase passes through the controller's RAM once during setup (to show it to the owner). The
  SE050 key itself is non-exportable afterwards.
- **OTP provisioning is one-way.** Secure boot enable, the boot-key hash, the SCP03 keys and (if chosen)
  debug disable cannot be undone or rekeyed. A lost signing key means no further firmware for every
  provisioned board; a leaked one means anyone can sign for them. A lost SCP03 key set means that
  controller+chip pair is scrap.
- **The SCP03 keys are readable by whatever firmware the secure boot trusts.** They sit in OTP rows that
  secure code can read (non-secure and bootloader access are locked). The protection is exactly as good
  as the signing key's secrecy and the absence of bugs in this firmware. There is no second secret.
- **The PIN rhythm has little entropy** (≤ 2^7 patterns per length, 4–8 taps). It is a 5-attempt lock, not
  a password; its strength is the chip's counter. Someone watching you tap learns it.
- **Bench validation is in progress.** The firmware's hardware paths (secure boot, the SE050 channel, the
  in-chip PIN) are built against the vendor SDKs and validated on the bench before any unit ships; see
  the README.
- No attestation: the daemon cannot distinguish our signed firmware from a controller that was never
  provisioned (an unprovisioned Pico 2 runs unsigned images). `hello` reports `dev_keys:true` when the
  SCP03 keys came from the build rather than OTP; the daemon must refuse production mode on it.
- The decoy wallet is only convincing if it holds a believable balance. That is the owner's job.

## Reporting

Open an issue or write to the address in the repository profile. We prefer fixes to disclosures and will
credit you.
