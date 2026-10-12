/* Pincer1 - Bitcoin PSBT (BIP-174, v0) parser, analysis against the owner's P2WPKH key, BIP-143 sighash,
 * partial-signature insertion and BIP-322 "simple" message signing. PURE LOGIC, no hardware.
 *
 * What the controller holds: ONE native-segwit key, m/84'/0'/0'/0/0 (inside the SE050), and the public
 * account node m/84'/0'/0' (xpub, in flash) so that change outputs can be recognised by their key path
 * (m/84'/0'/0'/1/i) and verified by re-deriving the child public key. Bounds: <= 8 inputs, <= 8 outputs,
 * <= BTCPSBT_MAX_BYTES on the wire; every signing digest is 32 bytes.
 *
 * Owner input = has a bip32_derivation entry whose pubkey is the controller's key AND whose path is
 * 84'/0'/0'/0/0 AND whose witness_utxo pays to 0014<hash160(pubkey)>. Any input that is not an owner
 * input makes the PSBT unsignable here (BTCPSBT_E_FOREIGN_INPUT): the controller refuses to sign into a
 * transaction whose other inputs it cannot account for. */
#ifndef PINCER_BTCPSBT_H
#define PINCER_BTCPSBT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bip32.h"

#define BTCPSBT_MAX_BYTES 4096
#define BTCPSBT_MAX_INPUTS 8
#define BTCPSBT_MAX_OUTPUTS 8
#define BTCPSBT_MAX_SCRIPT 42        /* longest standard scriptPubKey: witness v0..16 with 40-byte program */
#define BTCPSBT_MAX_CHANGE_INDEX 100000u
#define BTCPSBT_PARTIAL_SIG_MAX (1 + 1 + 1 + 33 + 1 + 73) /* keylen key(type+pubkey) vallen DER+sighash */
#define BTCPSBT_MAX_OUT_BYTES (BTCPSBT_MAX_BYTES + BTCPSBT_MAX_INPUTS * BTCPSBT_PARTIAL_SIG_MAX)

#define BTCPSBT_SIGHASH_ALL 0x01
#define BTCPSBT_SIGHASH_NONE 0x02
#define BTCPSBT_SIGHASH_SINGLE 0x03
#define BTCPSBT_SIGHASH_ANYONECANPAY 0x80

/* Dangerous flags, named exactly as `claims.dangerous[]` strings. */
#define BTCPSBT_DANGER_HIGH_FEE (1u << 0)       /* "high_fee": fee >= 10 % of what leaves, or any fee when nothing leaves */
#define BTCPSBT_DANGER_UNKNOWN_CHANGE (1u << 1) /* "unknown_change": an output that is neither the claimed dest nor recognised change */
#define BTCPSBT_DANGER_SIGHASH_NOT_ALL (1u << 2) /* "sighash_not_all": an owner input signs with something other than ALL */
#define BTCPSBT_DANGER_ALL 0x07u

typedef struct {
    uint8_t txid[32];      /* as in the wire format (little-endian hash) */
    uint32_t vout;
    uint32_t sequence;
    uint64_t amount;       /* witness_utxo */
    uint8_t spk[BTCPSBT_MAX_SCRIPT];
    uint8_t spk_len;
    bool has_witness_utxo;
    uint32_t sighash;      /* 0 = absent (ALL) */
    bool is_owner;
    uint8_t pubkey[33];    /* from bip32_derivation */
    bool has_derivation;
    uint32_t path[5];
    uint8_t path_len;
    size_t map_off;        /* byte offset of this input's map in the PSBT */
    size_t map_sep;        /* offset of its 0x00 separator (partial sigs are inserted here) */
} btcpsbt_input_t;

typedef struct {
    uint64_t amount;
    uint8_t spk[BTCPSBT_MAX_SCRIPT];
    uint8_t spk_len;
    bool has_derivation;
    uint8_t pubkey[33];
    uint32_t path[5];
    uint8_t path_len;
    bool is_change;        /* recognised via the account xpub */
} btcpsbt_output_t;

typedef struct {
    int32_t version;
    uint32_t locktime;
    uint8_t n_in, n_out;
    btcpsbt_input_t in[BTCPSBT_MAX_INPUTS];
    btcpsbt_output_t out[BTCPSBT_MAX_OUTPUTS];
    size_t tx_off, tx_len;      /* unsigned tx inside the PSBT */
    size_t inputs_off;          /* start of the first input map */
    size_t outputs_off;         /* start of the first output map */
    /* analysis */
    uint8_t n_owner_inputs;
    uint64_t sats_in;
    uint64_t sats_out;          /* non-change outputs */
    uint64_t sats_change;
    uint64_t fee;
    bool has_dest;
    uint8_t dest_spk[BTCPSBT_MAX_SCRIPT];
    uint8_t dest_spk_len;
    uint32_t dangerous;         /* BTCPSBT_DANGER_* except unknown_change (policy decides, it needs claims.dest) */
    uint8_t n_non_change;
} btcpsbt_t;

enum {
    BTCPSBT_OK = 0,
    BTCPSBT_E_MAGIC = -1,
    BTCPSBT_E_FORMAT = -2,       /* malformed map / varint / tx */
    BTCPSBT_E_TOOMANY = -3,      /* inputs or outputs over the bound */
    BTCPSBT_E_VERSION = -4,      /* PSBT v2 or unsigned tx with witnesses / scriptSigs */
    BTCPSBT_E_SIZE = -5,
    BTCPSBT_E_FOREIGN_INPUT = -6, /* an input the owner's key does not control (analysis) */
    BTCPSBT_E_NO_UTXO = -7,       /* owner input without witness_utxo */
    BTCPSBT_E_AMOUNTS = -8,       /* outputs exceed inputs / overflow */
    BTCPSBT_E_SIGHASH = -9,       /* SIGHASH_NONE (never signed) or unknown value */
};

int btcpsbt_parse(const uint8_t *bytes, size_t len, btcpsbt_t *p);
/* Classifies inputs and outputs against the owner's leaf pubkey (compressed) and account xpub. */
int btcpsbt_analyze(btcpsbt_t *p, const uint8_t owner_pub33[33], const bip32_xpub_t *account);
/* BIP-143 digest for owner input `idx` with its sighash type. */
bool btcpsbt_sighash(const btcpsbt_t *p, const uint8_t *bytes, uint8_t idx, uint8_t digest[32]);
/* Copies the PSBT to `out`, inserting PSBT_IN_PARTIAL_SIG (pubkey -> DER || sighash byte) for every owner
 * input; `sigs[i]` is the r||s for input i (ignored for non-owner inputs). Returns the output length or 0. */
size_t btcpsbt_insert_sigs(const btcpsbt_t *p, const uint8_t *bytes, size_t len, const uint8_t (*sigs)[64],
                           uint8_t *out, size_t cap);
/* True when the bytes start with the PSBT magic (used to refuse disguised signMessage payloads). */
bool btcpsbt_looks_like_psbt(const uint8_t *bytes, size_t len);

/* BIP-322 simple signature for a P2WPKH address: the digest to sign (BIP-143 sighash ALL of to_sign),
 * and the witness stack `02 <sig+01> <pubkey>` the daemon base64-encodes. */
void btcpsbt_bip322_digest(const uint8_t *msg, size_t len, const uint8_t pkh[20], uint8_t digest[32]);
/* Fills the to_spend txid and the message hash too (for tests / explanation). */
void btcpsbt_bip322_hashes(const uint8_t *msg, size_t len, const uint8_t pkh[20], uint8_t message_hash[32],
                           uint8_t to_spend_txid[32], uint8_t to_sign_txid[32]);
size_t btcpsbt_bip322_witness(const uint8_t sig[64], const uint8_t pub33[33], uint8_t *out, size_t cap);

const char *btcpsbt_danger_name(uint32_t bit);
uint32_t btcpsbt_danger_from_name(const char *name);
#endif
