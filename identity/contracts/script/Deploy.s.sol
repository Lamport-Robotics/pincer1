// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Script, console} from "forge-std/Script.sol";
import {Pincer1Units} from "../src/Pincer1Units.sol";

/// forge script script/Deploy.s.sol --rpc-url agung --broadcast --private-key $DEPLOYER_KEY
///   REGISTRY  peaq MachineRegistry on that chain (agung 0x538da35489B1F0035799a8351E67835ae4ABb377, peaq 0x64b93Cc29b251fAFa83BD110cDB1C24207f85536)
///   ATTESTER  the activation service's signing address
contract Deploy is Script {
    function run() external {
        address registry = vm.envAddress("REGISTRY");
        address attester = vm.envAddress("ATTESTER");
        vm.startBroadcast();
        Pincer1Units units = new Pincer1Units(registry, attester);
        vm.stopBroadcast();
        console.log("Pincer1Units", address(units));
        console.log("registry", registry);
        console.log("attester", attester);
    }
}
