// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Stand-ins for the peaq Economics 2.0 contracts the robot talks to, with the same selectors and events, so the
/// whole activation runs on anvil in tests. Nothing here is deployed anywhere real.

struct VerificationMethod {
    string id;
    string methodType;
    address controller;
    string publicKeyMultibase;
}

struct ServiceEndpoint {
    string id;
    string serviceType;
    string serviceEndpoint;
}

struct DIDDocument {
    string id;
    address controller;
    VerificationMethod[] verificationMethods;
    uint256[] authentication;
    ServiceEndpoint[] serviceEndpoints;
}

/// The native PEAQ precompile's ERC-20 face (0x…0809 on peaq). Here: a plain token the test mints.
contract MockPeaqToken {
    string public constant name = "peaq";
    string public constant symbol = "PEAQ";
    uint8 public constant decimals = 18;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);

    function mint(address to, uint256 amount) external {
        balanceOf[to] += amount;
        emit Transfer(address(0), to, amount);
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        emit Approval(msg.sender, spender, amount);
        return true;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        uint256 a = allowance[from][msg.sender];
        require(a >= amount, "allowance");
        if (a != type(uint256).max) allowance[from][msg.sender] = a - amount;
        _move(from, to, amount);
        return true;
    }

    function _move(address from, address to, uint256 amount) private {
        require(balanceOf[from] >= amount, "balance");
        balanceOf[from] -= amount;
        balanceOf[to] += amount;
        emit Transfer(from, to, amount);
    }
}

contract MockMachineRegistry {
    mapping(uint256 => address) private _owner;
    mapping(uint256 => address) public controllerOf;
    mapping(uint256 => string) public machineTypeOf;
    mapping(uint256 => bytes) public credentialSubjectOf;
    mapping(uint256 => address) public manufacturerOf;
    mapping(uint256 => ServiceEndpoint[]) private _services;
    mapping(uint256 => VerificationMethod[]) private _methods;
    address public lifecycle;

    event MachineMinted(uint256 indexed tokenId, address indexed to, address indexed controller, string machineType);
    event ServiceEndpointsUpdated(uint256 indexed tokenId);
    event Transfer(address indexed from, address indexed to, uint256 indexed tokenId);

    function setLifecycle(address l) external {
        lifecycle = l;
    }

    function computeTokenId(string calldata machineType, bytes calldata credentialSubject) public pure returns (uint256) {
        return uint256(keccak256(abi.encode(machineType, credentialSubject)));
    }

    function ownerOf(uint256 tokenId) external view returns (address) {
        address o = _owner[tokenId];
        require(o != address(0), "ERC721NonexistentToken");
        return o;
    }

    function didFromTokenId(uint256 tokenId) external pure returns (string memory) {
        return string.concat("did:peaq:", _dec(tokenId));
    }

    function serviceEndpointsOf(uint256 tokenId) external view returns (ServiceEndpoint[] memory) {
        return _services[tokenId];
    }

    function verificationMethodsOf(uint256 tokenId) external view returns (VerificationMethod[] memory) {
        return _methods[tokenId];
    }

    function mint(address to, DIDDocument calldata doc, string calldata machineType, bytes calldata credentialSubject, address manufacturer) external returns (uint256 tokenId) {
        require(msg.sender == lifecycle, "lifecycle");
        tokenId = computeTokenId(machineType, credentialSubject);
        require(_owner[tokenId] == address(0), "LocalMachineExists");
        _owner[tokenId] = to;
        controllerOf[tokenId] = doc.controller;
        machineTypeOf[tokenId] = machineType;
        credentialSubjectOf[tokenId] = credentialSubject;
        manufacturerOf[tokenId] = manufacturer;
        for (uint256 i = 0; i < doc.verificationMethods.length; i++) _methods[tokenId].push(doc.verificationMethods[i]);
        for (uint256 i = 0; i < doc.serviceEndpoints.length; i++) _services[tokenId].push(doc.serviceEndpoints[i]);
        emit Transfer(address(0), to, tokenId);
        emit MachineMinted(tokenId, to, doc.controller, machineType);
    }

    function setServiceEndpoints(uint256 tokenId, ServiceEndpoint[] calldata endpoints) external {
        require(msg.sender == _owner[tokenId] || msg.sender == controllerOf[tokenId], "not owner");
        delete _services[tokenId];
        for (uint256 i = 0; i < endpoints.length; i++) _services[tokenId].push(endpoints[i]);
        emit ServiceEndpointsUpdated(tokenId);
    }

    event ControllerUpdated(uint256 indexed tokenId, address indexed previous, address indexed controller);

    function setController(uint256 tokenId, address controller) external {
        require(msg.sender == _owner[tokenId], "not owner");
        emit ControllerUpdated(tokenId, controllerOf[tokenId], controller);
        controllerOf[tokenId] = controller;
    }

    function transferFrom(address from, address to, uint256 tokenId) external {
        require(_owner[tokenId] == from && msg.sender == from, "not owner");
        _owner[tokenId] = to;
        emit Transfer(from, to, tokenId);
    }

    function _dec(uint256 v) private pure returns (string memory) {
        if (v == 0) return "0";
        uint256 n = v;
        uint256 len;
        while (n != 0) {
            len++;
            n /= 10;
        }
        bytes memory b = new bytes(len);
        while (v != 0) {
            b[--len] = bytes1(uint8(48 + (v % 10)));
            v /= 10;
        }
        return string(b);
    }
}

contract MockMachineSubscription {
    MockPeaqToken public immutable token;
    address public lifecycle;
    uint256[3] public bond = [0.4 ether, 0.8 ether, 1.2 ether];
    mapping(address => uint256) public pendingVoucherCredit;
    struct Sub {
        uint8 tier;
        uint256 bonded;
        uint64 periodStart;
        uint64 periodEnd;
    }
    mapping(uint256 => Sub) public subscriptions;

    event Activated(uint256 indexed machineId, address indexed operator, uint8 tier, uint256 bondedAmount, uint64 periodStart, uint64 periodEnd);

    constructor(MockPeaqToken t) {
        token = t;
    }

    function setLifecycle(address l) external {
        lifecycle = l;
    }

    function requiredPeaqAmount(uint8 tier) external view returns (uint256) {
        return bond[tier];
    }

    function activate(uint256 machineId, address payer, uint8 tier) external {
        require(msg.sender == lifecycle, "lifecycle");
        uint256 amount = bond[tier];
        token.transferFrom(payer, address(this), amount);
        subscriptions[machineId] = Sub(tier, amount, uint64(block.timestamp), uint64(block.timestamp + 365 days));
        emit Activated(machineId, payer, tier, amount, uint64(block.timestamp), uint64(block.timestamp + 365 days));
    }
}

contract MockMachineStateAndSync {
    MockMachineRegistry public immutable registry;
    MockMachineSubscription public immutable subscription;

    event MachineOnboarded(uint256 indexed machineId, address indexed operator);

    constructor(MockMachineRegistry r, MockMachineSubscription s) {
        registry = r;
        subscription = s;
    }

    function activateMachine(DIDDocument calldata doc, string calldata machineType_, bytes calldata credentialSubject_, address manufacturer_, uint8 tier) external returns (uint256 machineId) {
        machineId = registry.mint(msg.sender, doc, machineType_, credentialSubject_, manufacturer_);
        subscription.activate(machineId, msg.sender, tier);
        emit MachineOnboarded(machineId, msg.sender);
    }
}
