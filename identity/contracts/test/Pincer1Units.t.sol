// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Test} from "forge-std/Test.sol";
import {Pincer1Units} from "../src/Pincer1Units.sol";
import {MockPeaqToken, MockMachineRegistry, MockMachineSubscription, MockMachineStateAndSync, DIDDocument, VerificationMethod, ServiceEndpoint} from "../src/mocks/MockPeaq.sol";

contract Pincer1UnitsTest is Test {
    MockPeaqToken token;
    MockMachineRegistry registry;
    MockMachineSubscription subscription;
    MockMachineStateAndSync lifecycle;
    Pincer1Units units;

    uint256 attesterKey = 0xA11CE;
    address attester = vm.addr(attesterKey);
    uint256 robotKey = 0xB0B;
    address robot = vm.addr(robotKey);
    address lamport = address(0x1a3);

    string serial = "P1-0001";

    function setUp() public {
        token = new MockPeaqToken();
        registry = new MockMachineRegistry();
        subscription = new MockMachineSubscription(token);
        lifecycle = new MockMachineStateAndSync(registry, subscription);
        registry.setLifecycle(address(lifecycle));
        subscription.setLifecycle(address(lifecycle));
        vm.prank(lamport);
        units = new Pincer1Units(address(registry), attester);
    }

    function _doc(address controller) internal pure returns (DIDDocument memory d) {
        d.controller = controller;
        d.verificationMethods = new VerificationMethod[](1);
        d.verificationMethods[0] = VerificationMethod("#key-1", "EcdsaSecp256k1VerificationKey2019", controller, "zQ3s");
        d.authentication = new uint256[](1);
        d.serviceEndpoints = new ServiceEndpoint[](1);
        d.serviceEndpoints[0] = ServiceEndpoint("#firmware", "Firmware", "0.5.1");
    }

    /// The robot's side of activation: bond, peaq activation, then our attestation.
    function _activate(address who, uint256 key) internal returns (uint256 id) {
        token.mint(who, 1 ether);
        vm.startPrank(who);
        token.approve(address(subscription), 0.4 ether);
        id = lifecycle.activateMachine(_doc(who), "pincer1", bytes(serial), lamport, 0);
        vm.stopPrank();
        assertEq(registry.ownerOf(id), who);
        assertEq(id, units.machineIdOf(serial));
        bytes memory sig = _sign(attesterKey, units.attestationDigest(id, serial, who));
        vm.prank(who);
        uint256 got = units.attest(serial, "0.5.1", sig);
        assertEq(got, id);
        key; // silence
    }

    function _sign(uint256 key, bytes32 digest) internal pure returns (bytes memory) {
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(key, digest);
        return abi.encodePacked(r, s, v);
    }

    function test_attest_records_unit() public {
        uint256 id = _activate(robot, robotKey);
        (string memory s, address m, string memory fw, uint64 at, uint64 seen, bool revoked) = units.units(id);
        assertEq(s, serial);
        assertEq(m, robot);
        assertEq(fw, "0.5.1");
        assertGt(at, 0);
        assertEq(seen, at);
        assertFalse(revoked);
        assertEq(units.unitCount(), 1);
        (uint256 id2, Pincer1Units.Unit memory u) = units.unitOf(serial);
        assertEq(id2, id);
        assertEq(u.machine, robot);
    }

    function test_attest_requires_machine_nft() public {
        // no peaq activation yet: ownerOf reverts inside the registry
        bytes memory sig = _sign(attesterKey, units.attestationDigest(units.machineIdOf(serial), serial, robot));
        vm.prank(robot);
        vm.expectRevert(bytes("ERC721NonexistentToken"));
        units.attest(serial, "0.5.1", sig);
    }

    function test_attest_rejects_wrong_signer_and_wrong_machine() public {
        token.mint(robot, 1 ether);
        vm.startPrank(robot);
        token.approve(address(subscription), 0.4 ether);
        uint256 id = lifecycle.activateMachine(_doc(robot), "pincer1", bytes(serial), lamport, 0);
        vm.stopPrank();
        // signed by someone else
        bytes memory bad = _sign(0xDEAD, units.attestationDigest(id, serial, robot));
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.BadSignature.selector);
        units.attest(serial, "0.5.1", bad);
        // signed for a different robot address
        bytes memory other = _sign(attesterKey, units.attestationDigest(id, serial, address(0xBEEF)));
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.BadSignature.selector);
        units.attest(serial, "0.5.1", other);
        // the right signature, from the wrong caller
        bytes memory good = _sign(attesterKey, units.attestationDigest(id, serial, robot));
        vm.prank(address(0xBEEF));
        vm.expectRevert(Pincer1Units.NotMachine.selector);
        units.attest(serial, "0.5.1", good);
        // once
        vm.prank(robot);
        units.attest(serial, "0.5.1", good);
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.AlreadyAttested.selector);
        units.attest(serial, "0.5.1", good);
    }

    function test_firmware_and_heartbeat_by_machine_only() public {
        uint256 id = _activate(robot, robotKey);
        vm.warp(block.timestamp + 3 days);
        vm.prank(robot);
        vm.expectEmit(true, false, false, true);
        emit Pincer1Units.Firmware(id, "0.5.1", "0.6.0");
        units.setFirmware(id, "0.6.0");
        vm.prank(robot);
        vm.expectEmit(true, false, false, true);
        emit Pincer1Units.Heartbeat(id, 72, 120_000);
        units.heartbeat(id, 72, 120_000);
        (,, string memory fw,, uint64 seen,) = units.units(id);
        assertEq(fw, "0.6.0");
        assertEq(seen, block.timestamp);
        vm.prank(address(0xBEEF));
        vm.expectRevert(Pincer1Units.NotMachine.selector);
        units.heartbeat(id, 1, 1);
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.BadFirmware.selector);
        units.setFirmware(id, "");
    }

    function test_nft_transfer_moves_write_rights() public {
        uint256 id = _activate(robot, robotKey);
        address buyer = address(0xB0BA);
        vm.prank(robot);
        registry.transferFrom(robot, buyer, id);
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.NotMachine.selector);
        units.heartbeat(id, 1, 1);
        vm.prank(buyer);
        units.heartbeat(id, 1, 1);
    }

    function test_revoke_and_admin() public {
        uint256 id = _activate(robot, robotKey);
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.NotOwner.selector);
        units.revoke(id, "x");
        vm.prank(lamport);
        units.revoke(id, "key leaked");
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.UnitRevoked.selector);
        units.heartbeat(id, 1, 1);
        vm.prank(lamport);
        units.setAttester(address(0xA77));
        assertEq(units.attester(), address(0xA77));
        vm.prank(lamport);
        units.transferOwnership(address(0x0A));
        assertEq(units.owner(), address(0x0A));
    }

    function test_unattested_cannot_write() public {
        vm.prank(robot);
        vm.expectRevert(Pincer1Units.NotAttested.selector);
        units.heartbeat(1, 1, 1);
    }
}
