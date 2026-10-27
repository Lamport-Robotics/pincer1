/* Pincer1 - flash persistence. PURE LOGIC over a tiny flash-ops interface (mocked on the host).
 * Layout: two regions of two sectors each at the end of flash; in each region records alternate between
 * the sectors so a power loss during a write never destroys the previous good record. Newest valid
 * CRC + highest seq wins.
 *
 *   main region        (last 8 KB)      1280-byte record v6: consent state (3 chains), pubkeys + addresses,
 *                                       BTC account xpub, soft-signer blobs, rollover uptime, owner routers,
 *                                       duress rhythm (flag; soft-signer salted hash)
 *   inheritance region (8 KB before it) 1280-byte record v1: powered-uptime counter + inheritance
 *                                       arrangement + the signed transaction. Written hourly and on
 *                                       inheritance changes; NEVER written by the PIN wipe. */
#ifndef PINCER_STORAGE_H
#define PINCER_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bip32.h"
#include "consent.h"

#define STORAGE_SECTOR_SIZE 4096u
#define STORAGE_PAGE_SIZE 256u
#define STORAGE_REGION_SIZE (2u * STORAGE_SECTOR_SIZE)
#define STORAGE_RECORD_SIZE 1536u /* multiple of the page size, <= one sector */
#define STORAGE_MAGIC 0x31524350u /* "PCR1" */
#define STORAGE_VERSION 8u /* v8: + last accepted host time; record carries an HMAC after the CRC */
#define STORAGE_MAC_LEN 32u

#define STORAGE_INH_REGION_SIZE (2u * STORAGE_SECTOR_SIZE)
#define STORAGE_INH_RECORD_SIZE 1280u /* multiple of the page size, <= one sector */
#define STORAGE_INH_MAGIC 0x31494350u /* "PCI1" */
#define STORAGE_INH_VERSION 2u /* v2: record carries an HMAC after the CRC */

typedef struct {
    /* Offsets are relative to the start of the storage region. */
    int (*read)(uint32_t off, void *buf, size_t len);
    int (*erase_sector)(uint32_t off);                        /* off is sector aligned */
    int (*program)(uint32_t off, const void *buf, size_t len); /* off page aligned, len multiple of page */
} storage_flash_t;

typedef struct {
    consent_persist_t consent;
    uint8_t pubkey[2][32];      /* slot 0 real, slot 1 decoy; zero until init */
    uint8_t signer_blob[2][32]; /* software signer ed25519 seeds (dev boards only); zero with the SE050 */
    uint8_t pin_hash[32];       /* software signer PIN hash (dev boards only); zero with the SE050 */
    uint8_t evm_addr[2][20];    /* keccak address of m/44'/60'/0'/0/0 per slot; zero until init */
    uint8_t btc_pkh[2][20];     /* hash160 of m/84'/0'/0'/0/0 per slot (bc1q address) */
    uint8_t secp_blob[2][2][32]; /* software signer secp256k1 keys [slot][evm,btc] (dev boards only) */
    uint8_t duress_hash[32];     /* software signer: salted hash of the duress rhythm (dev boards only); zero with the SE050 */
    uint8_t duress_salt[16];
} storage_data_t;

typedef struct {
    const storage_flash_t *fl;
    uint32_t seq;       /* seq of the newest valid record */
    int newest_slot;    /* -1 none, else 0/1 */
    uint8_t key[32];    /* record MAC key (storage_set_key); all-zero until set */
} storage_t;

/* Inheritance region payload: timer + arrangement (consent.h) + the once-signed transaction. */
typedef struct {
    consent_inh_persist_t ih;
    uint16_t msg_len;                     /* 0 = no transaction stored */
    uint8_t msg[CONSENT_INH_MSG_MAX];     /* serialized Solana message, as signed */
    uint8_t sig[64];                      /* ed25519 signature by the real key */
} storage_inh_data_t;

void storage_init(storage_t *s, const storage_flash_t *fl);
/* Key for the record MAC (both regions). Derived from the device secrets at boot (secrets_storage_key);
 * a record written under another key, or edited through the bootloader, does not load. */
void storage_set_key(storage_t *s, const uint8_t key[32]);
/* Loads the newest valid record. Returns false (and zeroes `d`) when flash holds nothing usable. */
bool storage_load(storage_t *s, storage_data_t *d);
/* Writes a new record into the other sector. Returns 0 or -1. */
int storage_save(storage_t *s, const storage_data_t *d);
/* Serialisation is exposed for tests. */
size_t storage_encode(const storage_data_t *d, uint32_t seq, const uint8_t key[32], uint8_t *out, size_t cap);
bool storage_decode(const uint8_t *in, size_t len, const uint8_t key[32], storage_data_t *d, uint32_t *seq);

/* ---- inheritance region: same engine, its own flash ops (offsets relative to that region) ---- */
void storage_inh_init(storage_t *s, const storage_flash_t *fl);
bool storage_inh_load(storage_t *s, storage_inh_data_t *d);
int storage_inh_save(storage_t *s, const storage_inh_data_t *d);
size_t storage_inh_encode(const storage_inh_data_t *d, uint32_t seq, const uint8_t key[32], uint8_t *out, size_t cap);
bool storage_inh_decode(const uint8_t *in, size_t len, const uint8_t key[32], storage_inh_data_t *d, uint32_t *seq);
#endif
