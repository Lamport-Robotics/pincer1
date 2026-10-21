/* Pincer1 - firmware update engine. PURE LOGIC over a flash-ops interface (mocked on the host).
 *
 * The daemon streams a signed image into the inactive A/B partition: `begin` (size, sha256, version),
 * `chunk` (sequential, page-aligned writes), `end` (hash and image checks). What boots is still decided by
 * the RP2350 bootrom: it verifies the image signature against the OTP key hash and, on a flash-update
 * boot, runs the new image "try before you buy"; main.c reboots into it only after the owner's long touch
 * and the daemon confirms with `update_commit` once the new image answers `hello`. Nothing here trusts
 * the daemon: a bad hash, a short image, an unsigned or foreign image, an out-of-order chunk all leave the
 * active image untouched.
 */
#ifndef PINCER_UPDATE_H
#define PINCER_UPDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define UPDATE_PAGE_SIZE 256u
#define UPDATE_SECTOR_SIZE 4096u
#define UPDATE_CHUNK_MAX 1024u    /* bytes per `update_chunk` (a multiple of the page size) */
#define UPDATE_VERSION_MAX 16
#define UPDATE_HEADER_SCAN 4096u  /* the IMAGE_DEF block sits in the first 4 KiB of an RP2350 image */

typedef struct {
    uint32_t size;                                        /* bytes the partition can hold */
    int (*erase)(uint32_t off);                           /* one sector at `off` (sector aligned) */
    int (*program)(uint32_t off, const void *buf, size_t len); /* page aligned, len multiple of a page */
    int (*read)(uint32_t off, void *buf, size_t len);
} update_flash_t;

typedef enum {
    UPDATE_IDLE = 0,
    UPDATE_RECEIVING,
    UPDATE_VERIFIED, /* `end` passed: the image is in flash, waiting for the owner's touch */
} update_state_t;

typedef enum {
    UPDATE_OK = 0,
    UPDATE_E_STATE,     /* call out of order */
    UPDATE_E_SIZE,      /* zero, over the partition, or not page aligned */
    UPDATE_E_OFFSET,    /* chunk not at the expected offset */
    UPDATE_E_LENGTH,    /* chunk not page aligned (except the last) or over the chunk maximum */
    UPDATE_E_FLASH,     /* erase/program/read failed */
    UPDATE_E_HASH,      /* received bytes do not hash to what `begin` announced */
    UPDATE_E_IMAGE,     /* no RP2350 block, not an executable, not secure, or unsigned */
    UPDATE_E_VERSION,   /* version string too long or empty */
} update_rc_t;

typedef struct {
    const update_flash_t *fl;
    update_state_t state;
    uint32_t size;
    uint32_t received;
    uint8_t expect[32];
    char version[UPDATE_VERSION_MAX];
    sha256_ctx hash;
    uint8_t head[UPDATE_HEADER_SCAN]; /* copy of the first bytes for the image checks */
    uint32_t head_len;
} update_t;

void update_init(update_t *u, const update_flash_t *fl);
update_rc_t update_begin(update_t *u, uint32_t size, const uint8_t sha256[32], const char *version);
update_rc_t update_chunk(update_t *u, uint32_t off, const uint8_t *data, size_t len);
update_rc_t update_end(update_t *u);
void update_abort(update_t *u);
const char *update_rc_str(update_rc_t rc);

/* Image checks shared with `end` (exposed for the tests and the daemon-side mirror):
 * finds the first picobin block in `head`, requires an IMAGE_TYPE item saying EXE / secure / RP2350
 * and a SIGNATURE item. Returns UPDATE_OK or UPDATE_E_IMAGE. */
update_rc_t update_check_image(const uint8_t *head, size_t len);

/* Boot ledger: one flash sector that says "partition P (version V) is on trial". The image that boots
 * from P counts its boots in it (one bit per boot, no erase needed) and, past UPDATE_TRIAL_BOOTS without
 * an `update_commit`, invalidates itself so the bootrom falls back to the other partition. Committing
 * erases the ledger. Arming it is the last thing the old image does before rebooting into the new one. */
#define UPDATE_TRIAL_BOOTS 3
#define UPDATE_LEDGER_MAGIC 0x4c444e50u /* "PNDL" */

typedef struct {
    bool armed;
    uint8_t partition;
    char version[UPDATE_VERSION_MAX];
    uint8_t boots; /* boots counted so far under this ledger */
} update_ledger_t;

void update_ledger_read(const update_flash_t *ledger, update_ledger_t *out);
int update_ledger_arm(const update_flash_t *ledger, uint8_t partition, const char *version);
/* Counts one more boot; returns the new count (0 when the ledger is not armed). */
uint8_t update_ledger_boot(const update_flash_t *ledger);
int update_ledger_clear(const update_flash_t *ledger);

#endif
