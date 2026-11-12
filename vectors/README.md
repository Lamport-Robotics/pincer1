# vectors

Real mainnet transactions and what a person must see before signing them. One JSON file each, read by
`daemon/test/vectors.test.ts` (the chain adapters) and `firmware/test/test_vectors.c` (the controller's
parsers), so the two sides are held to the same numbers.

```
solana/      system and token transfers, a jupiter route, a durable-nonce transaction, programs the controller does not know
solana_msg/  signMessage payloads. Never signed: the Privacy Rings key-derivation message (enveloped, bare, buried in a text), a real version 1 transaction message, raw digests, spaceless strings, text the display cannot draw, off-chain messages for another key. Signed: sign-in texts and off-chain messages (v0, v1) for the wallet's key, with the text the screen must show
evm/         ether transfers (legacy and type 2), erc-20 transfer / approve / transferFrom, a uniswap call, a contract creation, setApprovalForAll
bitcoin/     p2wpkh spends rebuilt as unsigned psbts: change, several inputs, a fee past the limit, no change; a psbt v2 that must be refused
evm_typed/   eip-712: usdc permits (bounded and unlimited), a seaport listing, the eip's own example
build.mjs    fetches them again and writes the files; node vectors/build.mjs [name...]
```

Each file carries `source` (where the bytes came from), `bytes` (the signing payload: a Solana message
with the signatures stripped, an unsigned EVM payload re-serialised and checked against the on-chain hash,
a PSBT built from the explorer's prevouts with the spender's key named on every input), `owner` (the
signer's key, so the parsers take the signer's point of view) and `expect`.

`expect` is decoded from the node's or explorer's own view of the transaction, never from this
repository's parsers: amounts in the smallest unit as decimal strings, the recipient, the fee, the
programs or contract, and `warnings`, the names the controller must see declared (`docs/PROTOCOL.md` §1).
`controller` says whether the controller signs it as is, signs it once the warnings are declared, or
refuses it outright and why: SOL towards two recipients, more token transfers than the display lists,
a PSBT version it does not read, typed data with arrays or nested structs. Solana messages that use
lookup tables carry the tables' contents (`lookup_tables`) so the daemon can resolve them; the controller
cannot, and the firmware test checks that it says so instead of signing what it cannot see.

`solana_msg/` is the exception to "real mainnet": its files are built by hand from the published layout of
the message (each names its source), since the point is a message that must never be signed at all.

Adding one: put the hash in `build.mjs` with a note on what it is, run it, read the file against the
explorer before committing it. The expectations are the product's rules applied to the bytes; a parser
that disagrees is the thing to fix.
