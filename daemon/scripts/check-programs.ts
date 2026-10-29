/**
 * Every program id in PROGRAM_NAMES must be a live executable account on mainnet, otherwise the
 * explainer names something that does not exist and the risk filter trusts a hole. Reads RPC_URL
 * like the daemon does. Exits 1 and lists the offenders when any id is missing or not executable.
 */
import { PROGRAM_NAMES } from "../src/tx/known.js";

const rpc = process.env.RPC_URL ?? "https://api.mainnet-beta.solana.com";
const ids = Object.keys(PROGRAM_NAMES);

const res = await fetch(rpc, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "getMultipleAccounts", params: [ids, { encoding: "base64", dataSlice: { offset: 0, length: 0 } }] }),
});
if (!res.ok) throw new Error(`${rpc}: HTTP ${res.status}`);
const body = (await res.json()) as { result?: { value: ({ executable: boolean; owner: string } | null)[] }; error?: { message: string } };
if (body.error || !body.result) throw new Error(`${rpc}: ${body.error?.message ?? "no result"}`);

let bad = 0;
body.result.value.forEach((acct, i) => {
  const id = ids[i];
  const name = PROGRAM_NAMES[id];
  if (!acct) {
    bad++;
    console.log(`MISSING   ${id}  (${name})`);
  } else if (!acct.executable) {
    bad++;
    console.log(`NOT EXEC  ${id}  (${name}) owner ${acct.owner}`);
  } else {
    console.log(`ok        ${id}  ${name}`);
  }
});
console.log(bad ? `${bad} of ${ids.length} program ids are not live programs` : `all ${ids.length} program ids are live programs`);
process.exit(bad ? 1 : 0);
