/* Pincer1 - address derivation and text encoding for EVM and Bitcoin. PURE LOGIC. */
#ifndef PINCER_CHAINADDR_H
#define PINCER_CHAINADDR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EVM_ADDR_HEX_LEN 43 /* "0x" + 40 + NUL */
#define BTC_ADDR_MAX 91     /* bech32 limit 90 + NUL */

/* keccak256(X||Y)[12..32] */
void evm_address_from_pubkey(const uint8_t pub[64], uint8_t addr[20]);
/* EIP-55 mixed-case hex, "0x" prefixed. */
void evm_address_to_hex(const uint8_t addr[20], char out[EVM_ADDR_HEX_LEN]);
/* Accepts 0x + 40 hex (any case; a mixed-case string must satisfy EIP-55). */
bool evm_address_from_hex(const char *s, uint8_t addr[20]);

/* hash160(compressed pubkey) */
void btc_pkh_from_pubkey(const uint8_t pub[64], uint8_t pkh[20]);
/* 0x00 0x14 <pkh> */
void btc_p2wpkh_script(const uint8_t pkh[20], uint8_t script[22]);
/* bc1q... (mainnet) */
bool btc_p2wpkh_address(const uint8_t pkh[20], char out[BTC_ADDR_MAX]);
/* scriptPubKey -> address text: bech32/bech32m for witness programs, base58check for P2PKH/P2SH.
 * Returns false for scripts without a standard address form. */
bool btc_script_to_address(const uint8_t *script, size_t len, char out[BTC_ADDR_MAX]);
/* address text -> scriptPubKey (mainnet only). Returns the script length or 0. */
size_t btc_address_to_script(const char *addr, uint8_t script[42]);
#endif
