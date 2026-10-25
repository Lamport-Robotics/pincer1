#include "update.h"

#include "harness.h"

/* In-memory partition with flash semantics: erase -> 0xFF, program may only clear bits. */
#define PART_SIZE (64u * 1024u)
static uint8_t g_part[PART_SIZE];
static int g_erases, g_programs, g_fail_program;

static int mock_erase(uint32_t off)
{
    if (off % UPDATE_SECTOR_SIZE || off + UPDATE_SECTOR_SIZE > PART_SIZE) return -1;
    memset(g_part + off, 0xff, UPDATE_SECTOR_SIZE);
    g_erases++;
    return 0;
}
static int mock_program(uint32_t off, const void *buf, size_t len)
{
    size_t i;
    if (off % UPDATE_PAGE_SIZE || len % UPDATE_PAGE_SIZE || off + len > PART_SIZE) return -1;
    if (g_fail_program) return -1;
    for (i = 0; i < len; i++) g_part[off + i] &= ((const uint8_t *)buf)[i];
    g_programs++;
    return 0;
}
static int mock_read(uint32_t off, void *buf, size_t len)
{
    if (off + len > PART_SIZE) return -1;
    memcpy(buf, g_part + off, len);
    return 0;
}
static const update_flash_t g_fl = {PART_SIZE, mock_erase, mock_program, mock_read};

static void put32(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24; }

/* A plausible RP2350 image head: vector table, then a block with IMAGE_TYPE (exe, secure, rp2350),
 * an optional SIGNATURE item, LAST. `flags` lets the tests build bad ones. */
static int g_ver_major = -1, g_ver_minor = 0; /* <0: no VERSION item */
static void make_image(uint8_t *img, size_t size, bool exe_secure, bool with_sig)
{
    size_t i, p;
    for (i = 0; i < size; i++) img[i] = (uint8_t)(i * 7 + 3);
    p = 0x100; /* block after the vector table */
    put32(img + p, 0xffffded3u); p += 4;
    put32(img + p, 0x42u | (1u << 8) | ((exe_secure ? (0x1u | (0x2u << 4) | (0x1u << 12)) : 0x2u) << 16)); p += 4;
    if (g_ver_major >= 0) { /* VERSION item: size 2 words, major.minor in the next word */
        put32(img + p, 0x48u | (2u << 8)); p += 4;
        put32(img + p, ((uint32_t)g_ver_major << 16) | (uint32_t)g_ver_minor); p += 4;
    }
    if (with_sig) { /* signature item: 2-byte size, 2 words payload here (real ones are longer) */
        put32(img + p, 0x09u | (3u << 8)); p += 4;
        put32(img + p, 0x11111111u); p += 4;
        put32(img + p, 0x22222222u); p += 4;
    }
    put32(img + p, 0xffu | (1u << 8)); p += 4; /* LAST, size 1 word */
    put32(img + p, 0); p += 4;                 /* loop: next block offset */
    put32(img + p, 0xab123579u);
}

static void feed(update_t *u, const uint8_t *img, size_t size, update_rc_t *last)
{
    size_t off = 0;
    *last = UPDATE_OK;
    while (off < size && *last == UPDATE_OK) {
        size_t n = size - off < UPDATE_CHUNK_MAX ? size - off : UPDATE_CHUNK_MAX;
        *last = update_chunk(u, (uint32_t)off, img + off, n);
        off += n;
    }
}

static void test_happy_path(void)
{
    static uint8_t img[8192];
    uint8_t h[32];
    update_t u;
    update_rc_t rc;
    make_image(img, sizeof img, true, true);
    sha256(img, sizeof img, h);
    g_erases = g_programs = 0;
    update_init(&u, &g_fl);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    CHECK_EQ(g_erases, 2); /* 8 KiB = two sectors, erased up front */
    CHECK_EQ(u.state, UPDATE_RECEIVING);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(rc, UPDATE_OK);
    CHECK_EQ(g_programs, 8);
    CHECK_EQ(update_end(&u), UPDATE_OK);
    CHECK_EQ(u.state, UPDATE_VERIFIED);
    CHECK(memcmp(g_part, img, sizeof img) == 0);
    CHECK_STR(u.version, "0.5.0");
    /* nothing more is accepted once verified; a new begin needs an abort */
    CHECK_EQ(update_chunk(&u, 0, img, 256), UPDATE_E_STATE);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.1"), UPDATE_E_STATE);
    update_abort(&u);
    CHECK_EQ(u.state, UPDATE_IDLE);
}

static void test_ordering_and_sizes(void)
{
    static uint8_t img[4096];
    uint8_t h[32];
    update_t u;
    make_image(img, sizeof img, true, true);
    sha256(img, sizeof img, h);
    update_init(&u, &g_fl);
    CHECK_EQ(update_chunk(&u, 0, img, 256), UPDATE_E_STATE);
    CHECK_EQ(update_end(&u), UPDATE_E_STATE);
    CHECK_EQ(update_begin(&u, 0, h, "1"), UPDATE_E_SIZE);
    CHECK_EQ(update_begin(&u, PART_SIZE + 256, h, "1"), UPDATE_E_SIZE);
    CHECK_EQ(update_begin(&u, 1000, h, "1"), UPDATE_E_SIZE); /* not page aligned */
    CHECK_EQ(update_begin(&u, 4096, h, ""), UPDATE_E_VERSION);
    CHECK_EQ(update_begin(&u, 4096, h, "0123456789abcdef"), UPDATE_E_VERSION);
    CHECK_EQ(update_begin(&u, 4096, h, "0.5.0"), UPDATE_OK);
    CHECK_EQ(update_chunk(&u, 256, img + 256, 256), UPDATE_E_OFFSET); /* must start at 0 */
    CHECK_EQ(update_chunk(&u, 0, img, 100), UPDATE_E_LENGTH);
    CHECK_EQ(update_chunk(&u, 0, img, UPDATE_CHUNK_MAX + 256), UPDATE_E_LENGTH);
    CHECK_EQ(update_chunk(&u, 0, img, 1024), UPDATE_OK);
    CHECK_EQ(update_chunk(&u, 1024, img + 1024, 1024), UPDATE_OK);
    CHECK_EQ(update_chunk(&u, 1024, img + 1024, 1024), UPDATE_E_OFFSET); /* replay of a chunk */
    CHECK_EQ(update_end(&u), UPDATE_E_SIZE);                              /* short: 2 KiB of 4 */
    CHECK_EQ(u.state, UPDATE_IDLE);                                        /* aborted */
}

static void test_hash_mismatch_leaves_nothing_usable(void)
{
    static uint8_t img[4096];
    uint8_t h[32];
    update_t u;
    update_rc_t rc;
    make_image(img, sizeof img, true, true);
    sha256(img, sizeof img, h);
    h[5] ^= 1;
    update_init(&u, &g_fl);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(rc, UPDATE_OK);
    CHECK_EQ(update_end(&u), UPDATE_E_HASH);
    CHECK_EQ(u.state, UPDATE_IDLE);
}

static void test_image_checks(void)
{
    static uint8_t img[4096];
    uint8_t h[32];
    update_t u;
    update_rc_t rc;
    /* unsigned image */
    make_image(img, sizeof img, true, false);
    sha256(img, sizeof img, h);
    update_init(&u, &g_fl);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_E_IMAGE);
    /* a data image, signed */
    make_image(img, sizeof img, false, true);
    sha256(img, sizeof img, h);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_E_IMAGE);
    /* no block at all */
    memset(img, 0x5a, sizeof img);
    sha256(img, sizeof img, h);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_E_IMAGE);
    /* the check on its own */
    make_image(img, sizeof img, true, true);
    CHECK_EQ(update_check_image(img, 4096), UPDATE_OK);
    CHECK_EQ(update_check_image(img, 0x100 + 8), UPDATE_E_IMAGE); /* window ends before LAST */
}

static void test_flash_failure_aborts(void)
{
    static uint8_t img[4096];
    uint8_t h[32];
    update_t u;
    make_image(img, sizeof img, true, true);
    sha256(img, sizeof img, h);
    update_init(&u, &g_fl);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.0"), UPDATE_OK);
    g_fail_program = 1;
    CHECK_EQ(update_chunk(&u, 0, img, 1024), UPDATE_E_FLASH);
    g_fail_program = 0;
    CHECK_EQ(u.state, UPDATE_IDLE);
    CHECK_STR(update_rc_str(UPDATE_E_FLASH), "flash");
}

static uint8_t g_ledger[UPDATE_SECTOR_SIZE];
static int ledger_erase(uint32_t off) { if (off != 0) return -1; memset(g_ledger, 0xff, sizeof g_ledger); return 0; }
static int ledger_program(uint32_t off, const void *buf, size_t len)
{
    size_t i;
    if (off % UPDATE_PAGE_SIZE || len % UPDATE_PAGE_SIZE || off + len > sizeof g_ledger) return -1;
    for (i = 0; i < len; i++) g_ledger[off + i] &= ((const uint8_t *)buf)[i];
    return 0;
}
static int ledger_read(uint32_t off, void *buf, size_t len) { if (off + len > sizeof g_ledger) return -1; memcpy(buf, g_ledger + off, len); return 0; }
static const update_flash_t g_ledger_fl = {sizeof g_ledger, ledger_erase, ledger_program, ledger_read};

static void test_boot_ledger(void)
{
    update_ledger_t l;
    int i;
    memset(g_ledger, 0xff, sizeof g_ledger); /* blank flash: nothing armed */
    update_ledger_read(&g_ledger_fl, &l);
    CHECK(!l.armed);
    CHECK_EQ(update_ledger_boot(&g_ledger_fl), 0);
    CHECK_EQ(update_ledger_arm(&g_ledger_fl, 1, "0.5.0"), 0);
    update_ledger_read(&g_ledger_fl, &l);
    CHECK(l.armed);
    CHECK_EQ(l.partition, 1);
    CHECK_STR(l.version, "0.5.0");
    CHECK_EQ(l.boots, 0);
    for (i = 1; i <= UPDATE_TRIAL_BOOTS; i++) CHECK_EQ(update_ledger_boot(&g_ledger_fl), i); /* bit clears only, no erase */
    update_ledger_read(&g_ledger_fl, &l);
    CHECK_EQ(l.boots, UPDATE_TRIAL_BOOTS);
    CHECK_EQ(update_ledger_clear(&g_ledger_fl), 0);
    update_ledger_read(&g_ledger_fl, &l);
    CHECK(!l.armed);
    CHECK_EQ(update_ledger_arm(&g_ledger_fl, 0, ""), -1);
}

static void test_version_item_and_downgrade(void)
{
    static uint8_t img[4096];
    uint8_t h[32];
    update_t u;
    update_rc_t rc;
    update_image_info_t info;
    update_set_running_version(0, 5);
    /* stamped 0.6: read, and allowed over 0.5 */
    g_ver_major = 0; g_ver_minor = 6;
    make_image(img, sizeof img, true, true);
    CHECK_EQ(update_inspect_image(img, 4096, &info), UPDATE_OK);
    CHECK(info.has_version); CHECK_EQ(info.major, 0); CHECK_EQ(info.minor, 6);
    sha256(img, sizeof img, h);
    update_init(&u, &g_fl);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.6.0"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_OK);
    CHECK(u.has_version); CHECK_EQ(u.img_major, 0); CHECK_EQ(u.img_minor, 6);
    update_abort(&u);
    /* the same major.minor is fine (a patch release) */
    g_ver_minor = 5;
    make_image(img, sizeof img, true, true); sha256(img, sizeof img, h);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.5.3"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_OK);
    update_abort(&u);
    /* older: refused before anything is staged */
    g_ver_minor = 4;
    make_image(img, sizeof img, true, true); sha256(img, sizeof img, h);
    CHECK_EQ(update_begin(&u, sizeof img, h, "0.4.9"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_E_VERSION);
    CHECK_EQ(u.state, UPDATE_IDLE);
    /* no stamp at all: refused once the running image has one */
    g_ver_major = -1;
    make_image(img, sizeof img, true, true); sha256(img, sizeof img, h);
    CHECK_EQ(update_begin(&u, sizeof img, h, "9.9.9"), UPDATE_OK);
    feed(&u, img, sizeof img, &rc);
    CHECK_EQ(update_end(&u), UPDATE_E_VERSION);
    CHECK(update_version_allowed(true, 1, 0));
    CHECK(!update_version_allowed(true, 0, 4));
    CHECK(!update_version_allowed(false, 0, 0));
}

int main(void)
{
    RUN(test_boot_ledger);
    RUN(test_happy_path);
    RUN(test_ordering_and_sizes);
    RUN(test_hash_mismatch_leaves_nothing_usable);
    RUN(test_image_checks);
    RUN(test_flash_failure_aborts);
    RUN(test_version_item_and_downgrade); /* last: it pins the running version for the rest of the process */
    TEST_END();
}
