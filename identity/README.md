# identity

Every Pincer1 can be a machine on [peaq](https://www.peaq.xyz): its own key, a machine NFT, a DID, and a small public
record of what it is and that it is alive. The owner decides; nothing is written until they hold the touch pad.

```
robot ──long touch──▶ controller derives the identity key (secp256k1, same seed, not a wallet)
robot ──proof──▶ Lamport activation service ──▶ checks the serial, funds the address, signs the attestation
robot ──▶ peaq   approve(bond) · MachineStateAndSync.activateMachine   → machine NFT + DID (did:peaq:<id>)
robot ──▶ peaq   Pincer1Units.attest(serial, firmware, signature)      → the Lamport record
robot ──▶ peaq   setFirmware on every controller update, heartbeat once a week (uptime hours, steps)
```

What is public: the serial, the robot's peaq address, the firmware version, weekly uptime and step counts.
What never leaves the robot: the owner, their wallets, where the robot is, what it sees, what it signs.

## Pieces

- `contracts/` — `Pincer1Units.sol` (Foundry). One record per machine id, written by the robot, countersigned by
  Lamport's attester key (EIP-712). `setFirmware` and `heartbeat` are open to whoever holds the machine NFT, so a
  sold robot keeps reporting for its new owner. `mocks/MockPeaq.sol` are stand-ins for the peaq contracts with
  the same selectors, for tests. `forge test`.
- `service/` — the activation service (`server.mjs`, Node + viem). `POST /activate {serial, machine, firmware,
  chainId, message, signature}`: verifies the robot's EIP-191 proof, checks the serial against `serials.json`,
  tops the robot's address up to `FUND_WEI` (bond + a year of gas), returns the attestation. One key binding per
  serial, ever. It never holds a robot key and never calls peaq for the robot.
- `../daemon/src/identity/` — the robot side (`Identity`), and `peaq.ts` with the published contract addresses
  and ABI fragments. Transactions are legacy EIP-155, signed by the controller as `id_tx`.
- `../daemon/src/controller/` — the key policy. `id_tx`: zero value, chain id and `to` from the owner's enable
  list, fee ≤ 0.05 PEAQ, no touch. `id_msg`: EIP-191 over a message starting with `pincer1-identity:`, no touch.
  Enable and disable are long touches. The sim implements it; the firmware spec is PROTOCOL §1 "identity".

## peaq side

The machine id is `keccak256(abi.encode("pincer1", bytes(serial)))`, computed by `MachineRegistry.computeTokenId`,
so a serial printed on a robot leads to its NFT, its DID and its Pincer1Units record. Activation is Entry tier
(USD 0.02 a year on mainnet, 0.4 AGUNG on agung), bonded in PEAQ by the robot itself. The DID document carries
the robot's key (`#key-1`, `EcdsaSecp256k1VerificationKey2019`) and two service endpoints: `#firmware`, the
release page of the running controller firmware, and `#maker`, lamportrobotics.com.

| chain | id | MachineRegistry | MachineStateAndSync | MachineSubscription |
|---|---|---|---|---|
| agung (test) | 9990 | `0x538da35489B1F0035799a8351E67835ae4ABb377` | `0xA7D33726232aae30622643a18047f0Ca3e159da9` | `0xaDCc5dD8CD57E3198B881c3388F25D2fD69B5aDc` |
| peaq | 3338 | `0x64b93Cc29b251fAFa83BD110cDB1C24207f85536` | `0xcD1917FB2a56459AcA34FDD118e08776dE0890f2` | `0x9e37AD189c334C92e6B8a812Ca4c02f35Ac43895` |

Source: docs.peaq.xyz/peaqos/concepts/contracts. Pincer1Units addresses go here once deployed.

## Deploy

```bash
cd identity/contracts && forge build && forge test
REGISTRY=0x538da35489B1F0035799a8351E67835ae4ABb377 ATTESTER=<service address> \
  forge script script/Deploy.s.sol --rpc-url agung --broadcast --private-key $DEPLOYER_KEY
cd ../service && cp .env.example .env   # UNITS_CONTRACT, ATTESTER_KEY, SERIALS_FILE
node server.mjs
```

Robot: `IDENTITY_CHAIN=agung IDENTITY_UNITS=<Pincer1Units> IDENTITY_SERVICE_URL=https://… IDENTITY_MANUFACTURER=<Lamport address>`
in the daemon's `.env`, then `POST /identity/activate` (the robot asks for a long touch), `GET /identity` for the DID
and transaction hashes, `POST /identity/heartbeat` to send one now, `POST /identity/disable` to turn the key off.

## Test

`daemon/test/identity.test.ts` runs the whole path on anvil: deploys the mocks and Pincer1Units from the forge
artifacts, starts the service in-process, activates the sim controller's identity, then checks ownership, the
record, a heartbeat while the wallet is locked, a firmware change, the controller's refusals and the service's.
