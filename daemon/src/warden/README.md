# Warden engine (v0)

Reads a signature request before it is signed, says in one sentence what it will do, and picks how much
friction it deserves. Spec: "Lamport Warden: product spec".

Nothing here calls a model or the network. It composes what the daemon already has:

| Piece | Where | Job |
|---|---|---|
| Decoder | `../tx/parse.ts` | instructions, what leaves the wallet, dangers (assign, authority, approve, nonce) |
| Scam filter | `../risk/filter.ts` | the hard rules and their reasons |
| Explainer | `../tx/explain.ts` | the sentence, from decoded facts |
| Message rule | `../tx/parse.ts` `checkSignMessage` | what a `signMessage` may be |
| Owner memory | `memory.ts` | who the owner has paid, how often, how much |
| Verdict | `verdict.ts` | `allow`, `explain`, `confirm`, `block` |

```ts
const memory = OwnerMemory.from(stored);
const v = checkTransaction({ bytes, owner, memory, origin });
// v.level, v.sentence, v.reasons, v.confirm
// after the owner confirmed and the wallet signed:
recordConfirmed(memory, v.parsed!, nowUnix);
```

Rules that are fixed:

- Trust in an address grows only through `recordConfirmed`. There is no call for inbound transfers.
- A destination that shares the first and last four characters with an address the owner has paid, and is
  not that address, is blocked.
- A request for the Privacy Rings key-derivation signature is blocked unless the site is on the owner's list,
  and then it still needs an explicit confirmation.
- Version 1 transactions (SIMD-0385) are read like any other, with the priority fee taken from the header.
  The controller's parser reads them too (`firmware/src/solmsg.c`), from the same shared vectors.
- A Privacy Rings pool or registry call is blocked for now: the amounts are hidden and Warden does not
  describe what it cannot read.

The replay corpus and the gate ("every drain caught, at most 1 in 50 normal requests interrupted") are in
`test/warden.test.ts`.

In the daemon, `Signer` uses the history (`historyOf`) on every Solana request and records a send after the
owner's touch; the memory is stored per signing address in `DATA_DIR/warden-memory.json`.

Not here yet: the browser interceptor, simulation, an IndexedDB adapter, agent policy.
