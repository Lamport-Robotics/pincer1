// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// The two reads this contract needs from peaq's MachineRegistry (Economics 2.0).
interface IMachineRegistry {
    function ownerOf(uint256 tokenId) external view returns (address);
    function computeTokenId(string calldata machineType, bytes calldata credentialSubject) external view returns (uint256);
}

/// Pincer1 units on peaq.
///
/// Every robot is first a peaq machine: the robot's own key activates it in MachineRegistry, which mints the
/// machine NFT and stores the DID. That part is peaq's. What peaq cannot know is whether the machine is a real
/// Pincer1, so this contract adds one record per unit, written by the robot itself and countersigned by Lamport:
///
///   attest     once, with Lamport's EIP-712 signature over (machineId, serial, robot address)
///   setFirmware whenever the controller firmware changes
///   heartbeat  a weekly "still here": uptime hours and steps walked, nothing about the owner
///
/// The machine id is peaq's: keccak256(abi.encode("pincer1", bytes(serial))), so anyone can go from a serial
/// number printed on the robot to its NFT, its DID and this record. The robot pays its own gas.
contract Pincer1Units {
    string public constant MACHINE_TYPE = "pincer1";
    bytes32 private constant ATTESTATION_TYPEHASH = keccak256("Attestation(uint256 machineId,string serial,address machine)");
    bytes32 private constant DOMAIN_TYPEHASH = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");

    IMachineRegistry public immutable registry;
    bytes32 public immutable domainSeparator;

    address public owner;
    /// Signs attestations. A warm key at the activation service; rotate it with setAttester, revoke units it signed by mistake.
    address public attester;

    struct Unit {
        string serial;
        address machine;
        string firmware;
        uint64 attestedAt;
        uint64 lastSeen;
        bool revoked;
    }

    mapping(uint256 machineId => Unit) public units;
    uint256 public unitCount;

    event Attested(uint256 indexed machineId, string serial, address indexed machine, string firmware);
    event Firmware(uint256 indexed machineId, string previous, string version);
    event Heartbeat(uint256 indexed machineId, uint32 uptimeHours, uint64 steps);
    event Revoked(uint256 indexed machineId, string reason);
    event AttesterChanged(address indexed previous, address indexed attester);
    event OwnershipTransferred(address indexed previous, address indexed owner);

    error NotOwner();
    error NotMachine();
    error AlreadyAttested();
    error NotAttested();
    error UnitRevoked();
    error BadSignature();
    error BadFirmware();
    error ZeroAddress();

    modifier onlyOwner() {
        if (msg.sender != owner) revert NotOwner();
        _;
    }

    /// The unit exists, is not revoked, and the caller holds its machine NFT right now.
    modifier onlyMachine(uint256 machineId) {
        Unit storage u = units[machineId];
        if (u.attestedAt == 0) revert NotAttested();
        if (u.revoked) revert UnitRevoked();
        if (registry.ownerOf(machineId) != msg.sender) revert NotMachine();
        _;
    }

    constructor(address registry_, address attester_) {
        if (registry_ == address(0) || attester_ == address(0)) revert ZeroAddress();
        registry = IMachineRegistry(registry_);
        attester = attester_;
        owner = msg.sender;
        domainSeparator = keccak256(abi.encode(DOMAIN_TYPEHASH, keccak256("Pincer1Units"), keccak256("1"), block.chainid, address(this)));
        emit OwnershipTransferred(address(0), msg.sender);
        emit AttesterChanged(address(0), attester_);
    }

    // ── reads ──

    function machineIdOf(string calldata serial) public view returns (uint256) {
        return registry.computeTokenId(MACHINE_TYPE, bytes(serial));
    }

    function unitOf(string calldata serial) external view returns (uint256 machineId, Unit memory unit) {
        machineId = machineIdOf(serial);
        unit = units[machineId];
    }

    /// The digest Lamport signs. Exposed so the activation service and the robot compute the same bytes.
    function attestationDigest(uint256 machineId, string calldata serial, address machine) public view returns (bytes32) {
        bytes32 structHash = keccak256(abi.encode(ATTESTATION_TYPEHASH, machineId, keccak256(bytes(serial)), machine));
        return keccak256(abi.encodePacked("\x19\x01", domainSeparator, structHash));
    }

    // ── writes by the robot ──

    /// Called by the robot after peaq activation. `signature` is the attester's 65-byte r‖s‖v over attestationDigest.
    function attest(string calldata serial, string calldata firmware, bytes calldata signature) external returns (uint256 machineId) {
        machineId = machineIdOf(serial);
        if (registry.ownerOf(machineId) != msg.sender) revert NotMachine();
        Unit storage u = units[machineId];
        if (u.attestedAt != 0) revert AlreadyAttested();
        if (bytes(firmware).length == 0 || bytes(firmware).length > 32) revert BadFirmware();
        if (_recover(attestationDigest(machineId, serial, msg.sender), signature) != attester) revert BadSignature();
        u.serial = serial;
        u.machine = msg.sender;
        u.firmware = firmware;
        u.attestedAt = uint64(block.timestamp);
        u.lastSeen = uint64(block.timestamp);
        unitCount += 1;
        emit Attested(machineId, serial, msg.sender, firmware);
        emit Firmware(machineId, "", firmware);
    }

    function setFirmware(uint256 machineId, string calldata version) external onlyMachine(machineId) {
        if (bytes(version).length == 0 || bytes(version).length > 32) revert BadFirmware();
        Unit storage u = units[machineId];
        string memory previous = u.firmware;
        u.firmware = version;
        u.lastSeen = uint64(block.timestamp);
        emit Firmware(machineId, previous, version);
    }

    function heartbeat(uint256 machineId, uint32 uptimeHours, uint64 steps) external onlyMachine(machineId) {
        units[machineId].lastSeen = uint64(block.timestamp);
        emit Heartbeat(machineId, uptimeHours, steps);
    }

    // ── admin ──

    /// A unit whose key leaked or whose attestation was a mistake. Its record stays readable; it can no longer write.
    function revoke(uint256 machineId, string calldata reason) external onlyOwner {
        Unit storage u = units[machineId];
        if (u.attestedAt == 0) revert NotAttested();
        u.revoked = true;
        emit Revoked(machineId, reason);
    }

    function setAttester(address attester_) external onlyOwner {
        if (attester_ == address(0)) revert ZeroAddress();
        emit AttesterChanged(attester, attester_);
        attester = attester_;
    }

    function transferOwnership(address owner_) external onlyOwner {
        if (owner_ == address(0)) revert ZeroAddress();
        emit OwnershipTransferred(owner, owner_);
        owner = owner_;
    }

    // ── ecdsa ──

    function _recover(bytes32 digest, bytes calldata signature) private pure returns (address) {
        if (signature.length != 65) revert BadSignature();
        bytes32 r = bytes32(signature[0:32]);
        bytes32 s = bytes32(signature[32:64]);
        uint8 v = uint8(signature[64]);
        if (v < 27) v += 27;
        if (v != 27 && v != 28) revert BadSignature();
        // low-s only (EIP-2)
        if (uint256(s) > 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0) revert BadSignature();
        address signer = ecrecover(digest, v, r, s);
        if (signer == address(0)) revert BadSignature();
        return signer;
    }
}
