#include "update.h"

#include <string.h>

/* picobin block markers and items (pico-sdk boot/picobin.h) */
#define BLOCK_MARKER_START 0xffffded3u
#define BLOCK_MARKER_END 0xab123579u
#define ITEM_1BS_IMAGE_TYPE 0x42u
#define ITEM_SIGNATURE 0x09u
#define ITEM_2BS_LAST 0xffu
#define IMAGE_TYPE_TYPE_BITS 0x000fu
#define IMAGE_TYPE_EXE 0x1u
#define IMAGE_TYPE_SECURITY_BITS 0x0030u
#define IMAGE_TYPE_SECURITY_S (0x2u << 4)
#define IMAGE_TYPE_CHIP_BITS 0x7000u
#define IMAGE_TYPE_CHIP_RP2350 (0x1u << 12)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void update_init(update_t *u, const update_flash_t *fl)
{
    memset(u, 0, sizeof *u);
    u->fl = fl;
}

void update_abort(update_t *u)
{
    const update_flash_t *fl = u->fl;
    memset(u, 0, sizeof *u);
    u->fl = fl;
}

const char *update_rc_str(update_rc_t rc)
{
    switch (rc) {
    case UPDATE_OK: return "ok";
    case UPDATE_E_STATE: return "state";
    case UPDATE_E_SIZE: return "size";
    case UPDATE_E_OFFSET: return "offset";
    case UPDATE_E_LENGTH: return "length";
    case UPDATE_E_FLASH: return "flash";
    case UPDATE_E_HASH: return "hash";
    case UPDATE_E_IMAGE: return "image";
    case UPDATE_E_VERSION: return "version";
    }
    return "?";
}

update_rc_t update_begin(update_t *u, uint32_t size, const uint8_t sha256[32], const char *version)
{
    size_t vl;
    uint32_t off;
    if (u->state != UPDATE_IDLE) return UPDATE_E_STATE;
    if (size == 0 || size > u->fl->size || size % UPDATE_PAGE_SIZE) return UPDATE_E_SIZE;
    vl = version ? strlen(version) : 0;
    if (vl == 0 || vl >= UPDATE_VERSION_MAX) return UPDATE_E_VERSION;
    /* erase every sector the image will cover before the first byte lands */
    for (off = 0; off < size; off += UPDATE_SECTOR_SIZE)
        if (u->fl->erase(off) != 0) { update_abort(u); return UPDATE_E_FLASH; }
    u->size = size;
    u->received = 0;
    u->head_len = 0;
    memcpy(u->expect, sha256, 32);
    memcpy(u->version, version, vl + 1);
    sha256_init(&u->hash);
    u->state = UPDATE_RECEIVING;
    return UPDATE_OK;
}

update_rc_t update_chunk(update_t *u, uint32_t off, const uint8_t *data, size_t len)
{
    size_t keep;
    if (u->state != UPDATE_RECEIVING) return UPDATE_E_STATE;
    if (off != u->received) return UPDATE_E_OFFSET;
    if (len == 0 || len > UPDATE_CHUNK_MAX || len % UPDATE_PAGE_SIZE || off + len > u->size) return UPDATE_E_LENGTH;
    if (u->fl->program(off, data, len) != 0) { update_abort(u); return UPDATE_E_FLASH; }
    sha256_update(&u->hash, data, len);
    if (u->head_len < UPDATE_HEADER_SCAN) {
        keep = UPDATE_HEADER_SCAN - u->head_len;
        if (keep > len) keep = len;
        memcpy(u->head + u->head_len, data, keep);
        u->head_len += (uint32_t)keep;
    }
    u->received += (uint32_t)len;
    return UPDATE_OK;
}

update_rc_t update_check_image(const uint8_t *head, size_t len)
{
    size_t start, p;
    bool exe = false, signed_ = false;
    /* the block loop starts at a word-aligned marker somewhere in the first 4 KiB */
    for (start = 0; start + 8 <= len; start += 4)
        if (rd32(head + start) == BLOCK_MARKER_START) break;
    if (start + 8 > len) return UPDATE_E_IMAGE;
    p = start + 4;
    while (p + 4 <= len) {
        uint32_t item = rd32(head + p);
        uint8_t type = (uint8_t)(item & 0xff);
        uint32_t words;
        if (type == ITEM_2BS_LAST) break;
        if (type == ITEM_1BS_IMAGE_TYPE) {
            uint32_t flags = item >> 16;
            exe = (flags & IMAGE_TYPE_TYPE_BITS) == IMAGE_TYPE_EXE && (flags & IMAGE_TYPE_SECURITY_BITS) == IMAGE_TYPE_SECURITY_S &&
                  (flags & IMAGE_TYPE_CHIP_BITS) == IMAGE_TYPE_CHIP_RP2350;
        } else if (type == ITEM_SIGNATURE) {
            signed_ = true;
        }
        /* 1-byte-size items carry their word count in byte 1; 2-byte-size items in bytes 1..2 */
        words = (type & 0x80) ? (item >> 8) & 0xffff : (item >> 8) & 0xff;
        if (words == 0) words = 1;
        p += (size_t)words * 4;
    }
    if (p + 4 > len) return UPDATE_E_IMAGE; /* ran off the scanned window without a LAST item */
    return exe && signed_ ? UPDATE_OK : UPDATE_E_IMAGE;
}

update_rc_t update_end(update_t *u)
{
    uint8_t got[32];
    update_rc_t rc;
    if (u->state != UPDATE_RECEIVING) return UPDATE_E_STATE;
    if (u->received != u->size) { update_abort(u); return UPDATE_E_SIZE; }
    sha256_final(&u->hash, got);
    if (memcmp(got, u->expect, 32) != 0) { update_abort(u); return UPDATE_E_HASH; }
    rc = update_check_image(u->head, u->head_len);
    if (rc != UPDATE_OK) { update_abort(u); return rc; }
    /* read back the first page: what we hashed must be what flash holds */
    if (u->fl->read(0, got, sizeof got) != 0 || memcmp(got, u->head, sizeof got) != 0) { update_abort(u); return UPDATE_E_FLASH; }
    u->state = UPDATE_VERIFIED;
    return UPDATE_OK;
}

/* ---- boot ledger: page 0 of the ledger sector ----
 * bytes 0..3 magic, 4 partition, 5 reserved, 6..21 version (NUL padded), 32..63 boot counter as cleared bits */
#define LEDGER_COUNT_OFF 32u
#define LEDGER_COUNT_BYTES 32u

static int ledger_page(const update_flash_t *fl, uint8_t page[UPDATE_PAGE_SIZE])
{
    return fl->read(0, page, UPDATE_PAGE_SIZE);
}

static uint8_t count_cleared_bits(const uint8_t *p, size_t n)
{
    size_t i;
    unsigned b, cleared = 0;
    for (i = 0; i < n; i++)
        for (b = 0; b < 8; b++)
            if (!(p[i] & (1u << b))) cleared++;
    return cleared > 255 ? 255 : (uint8_t)cleared;
}

void update_ledger_read(const update_flash_t *ledger, update_ledger_t *out)
{
    uint8_t page[UPDATE_PAGE_SIZE];
    memset(out, 0, sizeof *out);
    if (ledger_page(ledger, page) != 0) return;
    if (rd32(page) != UPDATE_LEDGER_MAGIC) return;
    out->armed = true;
    out->partition = page[4];
    memcpy(out->version, page + 6, UPDATE_VERSION_MAX - 1);
    out->version[UPDATE_VERSION_MAX - 1] = 0;
    out->boots = count_cleared_bits(page + LEDGER_COUNT_OFF, LEDGER_COUNT_BYTES);
}

int update_ledger_arm(const update_flash_t *ledger, uint8_t partition, const char *version)
{
    uint8_t page[UPDATE_PAGE_SIZE];
    size_t vl = version ? strlen(version) : 0;
    if (vl == 0 || vl >= UPDATE_VERSION_MAX) return -1;
    if (ledger->erase(0) != 0) return -1;
    memset(page, 0xff, sizeof page);
    page[0] = UPDATE_LEDGER_MAGIC & 0xff;
    page[1] = (UPDATE_LEDGER_MAGIC >> 8) & 0xff;
    page[2] = (UPDATE_LEDGER_MAGIC >> 16) & 0xff;
    page[3] = UPDATE_LEDGER_MAGIC >> 24;
    page[4] = partition;
    page[5] = 0;
    memset(page + 6, 0, UPDATE_VERSION_MAX);
    memcpy(page + 6, version, vl);
    return ledger->program(0, page, sizeof page);
}

uint8_t update_ledger_boot(const update_flash_t *ledger)
{
    uint8_t page[UPDATE_PAGE_SIZE];
    size_t i;
    unsigned b;
    if (ledger_page(ledger, page) != 0 || rd32(page) != UPDATE_LEDGER_MAGIC) return 0;
    for (i = LEDGER_COUNT_OFF; i < LEDGER_COUNT_OFF + LEDGER_COUNT_BYTES; i++) {
        for (b = 0; b < 8; b++) {
            if (page[i] & (1u << b)) {
                page[i] &= (uint8_t)~(1u << b); /* only ever clears a bit: no erase needed */
                if (ledger->program(0, page, sizeof page) != 0) return 0;
                return count_cleared_bits(page + LEDGER_COUNT_OFF, LEDGER_COUNT_BYTES);
            }
        }
    }
    return 255; /* counter exhausted */
}

int update_ledger_clear(const update_flash_t *ledger)
{
    return ledger->erase(0);
}
