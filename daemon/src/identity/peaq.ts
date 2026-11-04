/**
 * peaq Economics 2.0: the addresses and the slice of each ABI the robot uses to become a machine.
 *
 * Addresses are the published deployment records (docs.peaq.xyz/peaqos/concepts/contracts). The ABI fragments
 * are copied from the peaqOS SDK's bundled JSON (@peaqos/peaq-os-sdk 0.12.0, Apache-2.0); the daemon speaks to
 * the contracts directly with viem because the SDK wants a raw private key and the robot's key never leaves
 * the controller.
 */
import type { Address, Hex } from "viem";
import { parseAbi } from "viem";

export interface PeaqDeployment {
  id: "peaq-mainnet" | "agung-2026-08-28";
  chainId: number;
  name: string;
  rpc: string;
  explorer: string;
  /** Native-token ERC-20 face; `approve` on it is what MachineSubscription pulls the bond through. */
  peaqToken: Address;
  machineRegistry: Address;
  machineStateAndSync: Address;
  machineSubscription: Address;
  /** Where machines.peaq.xyz / the MCR API read 2.0 machines (mainnet only today). */
  mcrApi?: string;
  /** Pincer1Units on this chain, when deployed (IDENTITY_UNITS overrides). */
  units?: Address;
}

export const PEAQ_TOKEN: Address = "0x0000000000000000000000000000000000000809";

export const DEPLOYMENTS: Record<number, PeaqDeployment> = {
  3338: {
    id: "peaq-mainnet", chainId: 3338, name: "peaq", rpc: "https://peaq-rpc.publicnode.com", explorer: "https://peaq.subscan.io",
    peaqToken: PEAQ_TOKEN,
    machineRegistry: "0x64b93Cc29b251fAFa83BD110cDB1C24207f85536",
    machineStateAndSync: "0xcD1917FB2a56459AcA34FDD118e08776dE0890f2",
    machineSubscription: "0x9e37AD189c334C92e6B8a812Ca4c02f35Ac43895",
    mcrApi: "https://mcr.peaq.xyz",
    units: "0x3e6A4a540344E844db6Ba1640Bf92DB927D60F9a",
  },
  9990: {
    id: "agung-2026-08-28", chainId: 9990, name: "agung", rpc: "https://peaq-agung.api.onfinality.io/public", explorer: "https://agung-testnet.subscan.io",
    peaqToken: PEAQ_TOKEN,
    machineRegistry: "0x538da35489B1F0035799a8351E67835ae4ABb377",
    machineStateAndSync: "0xA7D33726232aae30622643a18047f0Ca3e159da9",
    machineSubscription: "0xaDCc5dD8CD57E3198B881c3388F25D2fD69B5aDc",
  },
};

export const SUBSCRIPTION_TIER = { entry: 0, basic: 1, pro: 2 } as const;

export const peaqTokenAbi = parseAbi([
  "function balanceOf(address owner) view returns (uint256)",
  "function allowance(address owner, address spender) view returns (uint256)",
  "function approve(address spender, uint256 amount) returns (bool)",
]);

export const machineSubscriptionAbi = parseAbi([
  "function requiredPeaqAmount(uint8 tier) view returns (uint256)",
  "function pendingVoucherCredit(address operator) view returns (uint256)",
  "function subscriptions(uint256 machineId) view returns (uint8 tier, uint256 bonded, uint64 periodStart, uint64 periodEnd)",
  "event Activated(uint256 indexed machineId, address indexed operator, uint8 tier, uint256 bondedAmount, uint64 periodStart, uint64 periodEnd)",
]);

export const machineRegistryAbi = parseAbi([
  "struct VerificationMethod { string id; string methodType; address controller; string publicKeyMultibase; }",
  "struct ServiceEndpoint { string id; string serviceType; string serviceEndpoint; }",
  "function computeTokenId(string machineType, bytes credentialSubject) view returns (uint256)",
  "function ownerOf(uint256 tokenId) view returns (address)",
  "function controllerOf(uint256 tokenId) view returns (address)",
  "function didFromTokenId(uint256 tokenId) view returns (string)",
  "function serviceEndpointsOf(uint256 tokenId) view returns (ServiceEndpoint[])",
  "function verificationMethodsOf(uint256 tokenId) view returns (VerificationMethod[])",
  "function setServiceEndpoints(uint256 tokenId, ServiceEndpoint[] endpoints)",
  "function setController(uint256 tokenId, address controller)",
  "event ControllerUpdated(uint256 indexed tokenId, address indexed previous, address indexed controller)",
  "event MachineMinted(uint256 indexed tokenId, address indexed to, address indexed controller, string machineType)",
]);

export const machineStateAndSyncAbi = parseAbi([
  "struct VerificationMethod { string id; string methodType; address controller; string publicKeyMultibase; }",
  "struct ServiceEndpoint { string id; string serviceType; string serviceEndpoint; }",
  "struct DIDDocument { string id; address controller; VerificationMethod[] verificationMethods; uint256[] authentication; ServiceEndpoint[] serviceEndpoints; }",
  "function activateMachine(DIDDocument doc, string machineType_, bytes credentialSubject_, address manufacturer_, uint8 tier) returns (uint256 machineId)",
  "event MachineOnboarded(uint256 indexed machineId, address indexed operator)",
]);

/** Pincer1Units (identity/contracts/src/Pincer1Units.sol): Lamport's attestation and the robot's own log. */
export const pincer1UnitsAbi = parseAbi([
  "function attester() view returns (address)",
  "function machineIdOf(string serial) view returns (uint256)",
  "function units(uint256 machineId) view returns (string serial, address machine, string firmware, uint64 attestedAt, uint64 lastSeen, bool revoked)",
  "function attestationDigest(uint256 machineId, string serial, address machine) view returns (bytes32)",
  "function attest(string serial, string firmware, bytes signature) returns (uint256 machineId)",
  "function setFirmware(uint256 machineId, string version)",
  "function heartbeat(uint256 machineId, uint32 uptimeHours, uint64 steps)",
  "event Attested(uint256 indexed machineId, string serial, address indexed machine, string firmware)",
  "event Firmware(uint256 indexed machineId, string previous, string version)",
  "event Heartbeat(uint256 indexed machineId, uint32 uptimeHours, uint64 steps)",
]);

/** EIP-712 domain of Pincer1Units; the activation service signs with it, the contract checks it. */
export const attestationTypes = { Attestation: [{ name: "machineId", type: "uint256" }, { name: "serial", type: "string" }, { name: "machine", type: "address" }] } as const;
export const attestationDomain = (chainId: number, verifyingContract: Address) => ({ name: "Pincer1Units", version: "1", chainId, verifyingContract }) as const;

export const MACHINE_TYPE = "pincer1";
/** The credential subject is the serial as UTF-8, so machineId = keccak256(abi.encode("pincer1", bytes(serial))). */
export const credentialSubjectOf = (serial: string): Hex => ("0x" + Buffer.from(serial, "utf8").toString("hex")) as Hex;
