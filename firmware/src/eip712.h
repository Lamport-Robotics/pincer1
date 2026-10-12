/* Pincer1 - EIP-712 typed-data hashing on-device, for structs of PRIMITIVE types only. PURE LOGIC.
 *
 * Supported field types: address, bool, uintN / intN (N = 8..256 step 8), bytesN (1..32), bytes, string.
 * Arrays and nested structs are refused with EIP712_E_UNSUPPORTED ("unsupported_typed_data" to the
 * daemon): a full encoder is not worth its size in the trusted image, and the common high-value cases
 * (ERC-2612 permit, Permit2 single, exchange login challenges, 1inch/0x order hashes without nested
 * structs) are flat. The daemon still shows the user what it believes the message says; this module
 * makes sure the digest the daemon wants signed is the digest of the JSON it showed.
 *
 * Input is the cJSON tree of {"types":{...},"primaryType":"...","domain":{...},"message":{...}}.
 * Integer values may be JSON numbers (exact integers below 2^53), decimal strings or 0x-hex strings;
 * negative intN values as "-123". */
#ifndef PINCER_EIP712_H
#define PINCER_EIP712_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define EIP712_MAX_FIELDS 32
#define EIP712_MAX_TYPE_STR 1024

enum {
    EIP712_OK = 0,
    EIP712_E_MALFORMED = -1,   /* not the EIP-712 JSON shape, value does not fit its type */
    EIP712_E_UNSUPPORTED = -2, /* nested struct, array, unknown type, too many fields */
};

/* hashStruct(primaryType, message) for the named struct in `types`. */
int eip712_hash_struct(const cJSON *types, const char *type_name, const cJSON *data, uint8_t out[32]);
/* keccak(0x19 0x01 || hashStruct(EIP712Domain, domain) || hashStruct(primaryType, message)) */
int eip712_digest(const cJSON *typed_data, uint8_t digest[32]);
#endif
