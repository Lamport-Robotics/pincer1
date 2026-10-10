#include "storage.h"

#include "harness.h"
#include "util.h"

/* In-memory flash mock with real flash semantics: erase -> 0xFF, program may only clear bits. */
static uint8_t g_flash[STORAGE_REGION_SIZE];
static int g_erases, g_programs, g_fail_program;

static int mock_read(uint32_t off, void *buf, size_t len)
{
    if (off + len > sizeof g_flash) return -1;
    memcpy(buf, g_flash + off, len);
    return 0;
}
static int mock_erase(uint32_t off)
{
    if (off % STORAGE_SECTOR_SIZE || off + STORAGE_SECTOR_SIZE > sizeof g_flash) return -1;
    memset(g_flash + off, 0xff, STORAGE_SECTOR_SIZE);
    g_erases++;
    return 0;
}
static int mock_program(uint32_t off, const void *buf, size_t len)
{
    size_t i;
    if (off % STORAGE_PAGE_SIZE || len % STORAGE_PAGE_SIZE || off + len > sizeof g_flash) return -1;
    if (g_fail_program) { /* simulate power loss half way: only first page lands */
        for (i = 0; i < STORAGE_PAGE_SIZE; i++) g_flash[off + i] &= ((const uint8_t *)buf)[i];
        return -1;
    }
    for (i = 0; i < len; i++) g_flash[off + i] &= ((const uint8_t *)buf)[i];
    g_programs++;
    return 0;
}
static const storage_flash_t FL = {mock_read, mock_erase, mock_program};

static void fill(storage_data_t *d, int v)
{
    int i;
    memset(d, 0, sizeof *d);
    d->consent.initialized = true;
    d->consent.duress = (v & 1) != 0;
    d->consent.daily_limit = 2000000000ull + (uint64_t)v;
    d->consent.spent_today = 123456789ull * (uint64_t)v;
    d->consent.day = 20395u + (uint32_t)v;
    for (i = 0; i < 32; i++) {
        d->pubkey[0][i] = (uint8_t)(i + v);
        d->pubkey[1][i] = (uint8_t)(200 - i - v);
        d->signer_blob[0][i] = (uint8_t)(i * 3 + v);
    }
    d->consent.allowance.active = true;
    d->consent.allowance.lamports = 1000000000ull;
    d->consent.allowance.spent = 250000000ull;
    d->consent.allowance.max_per_tx = 200000000ull;
    d->consent.allowance.until_unix = 1791650000ull;
    d->consent.allowance.n_programs = 2;
    memset(d->consent.allowance.programs[0], 0xAA, 32);
    memset(d->consent.allowance.programs[1], 0xBB, 32);
    d->consent.pin_set = (v & 2) != 0;
    d->consent.pin_failures = (uint8_t)(v % 6);
    for (i = 0; i < 32; i++) d->pin_hash[i] = (uint8_t)(0x40 + i + v);
    /* v4: per-chain limits, addresses, account xpub, soft secp256k1 blobs */
    u256_from_dec("2000000000000000000", &d->consent.daily_limit_wei);
    d->consent.daily_limit_wei.w[3] = (uint64_t)v << 56; /* exercise the high limb */
    u256_from_u64(&d->consent.spent_today_wei, 123456789ull * (uint64_t)v);
    d->consent.daily_limit_sats = 300000ull + (uint64_t)v;
    d->consent.spent_today_sats = 17ull * (uint64_t)v;
    for (i = 0; i < 20; i++) {
        d->evm_addr[0][i] = (uint8_t)(0x90 + i + v);
        d->evm_addr[1][i] = (uint8_t)(0x70 - i - v);
        d->btc_pkh[0][i] = (uint8_t)(0xc0 + i + v);
        d->btc_pkh[1][i] = (uint8_t)(0x10 + i + v);
    }
    for (i = 0; i < 33; i++) { d->btc_xpub[0].pub[i] = (uint8_t)(2 + i * v); d->btc_xpub[1].pub[i] = (uint8_t)(3 + i + v); }
    for (i = 0; i < 32; i++) { d->btc_xpub[0].chain[i] = (uint8_t)(i ^ v); d->btc_xpub[1].chain[i] = (uint8_t)(~i + v); }
    for (i = 0; i < 32; i++) {
        d->secp_blob[0][0][i] = (uint8_t)(i * 5 + v); d->secp_blob[0][1][i] = (uint8_t)(i * 7 + v);
        d->secp_blob[1][0][i] = (uint8_t)(i * 11 + v); d->secp_blob[1][1][i] = (uint8_t)(i * 13 + v);
    }
    /* v6: duress rhythm flag + soft-signer salted hash */
    d->consent.duress_pin_set = (v & 4) != 0;
    for (i = 0; i < 32; i++) d->duress_hash[i] = (uint8_t)(0x80 + i * v);
    for (i = 0; i < 16; i++) d->duress_salt[i] = (uint8_t)(0xd0 + i + v);
    /* v5: rollover uptime + owner routers */
    d->consent.rollover_powered_seconds = 72000ull * (uint64_t)v + 5;
    d->consent.routers.n = (uint8_t)(v % (ROUTERS_MAX_OWNER + 1));
    for (i = 0; i < ROUTERS_MAX_OWNER; i++) {
        d->consent.routers.e[i].chain = (uint8_t)((i + v) & 1);
        memset(d->consent.routers.e[i].addr, 0x60 + i + v, 32);
    }
}

static void test_encode_decode(void)
{
    storage_data_t d, back;
    uint8_t buf[STORAGE_RECORD_SIZE];
    uint32_t seq = 0;
    size_t n;
    fill(&d, 3);
    n = storage_encode(&d, 42, buf, sizeof buf);
    CHECK(n > 0 && n <= STORAGE_RECORD_SIZE);
    CHECK(storage_decode(buf, sizeof buf, &back, &seq));
    CHECK_EQ(seq, 42);
    CHECK_MEM(&back.consent.allowance, &d.consent.allowance, sizeof d.consent.allowance);
    CHECK_U64(back.consent.daily_limit, d.consent.daily_limit);
    CHECK_U64(back.consent.spent_today, d.consent.spent_today);
    CHECK_EQ(back.consent.day, d.consent.day);
    CHECK_EQ(back.consent.duress, 1);
    CHECK_MEM(back.pubkey, d.pubkey, 64);
    CHECK_MEM(back.signer_blob, d.signer_blob, 64);
    CHECK_EQ(back.consent.pin_set, 1);
    CHECK_EQ(back.consent.pin_failures, 3);
    CHECK_MEM(back.pin_hash, d.pin_hash, 32);
    CHECK_EQ(u256_cmp(&back.consent.daily_limit_wei, &d.consent.daily_limit_wei), 0);
    CHECK_EQ(u256_cmp(&back.consent.spent_today_wei, &d.consent.spent_today_wei), 0);
    CHECK_U64(back.consent.daily_limit_sats, 300003);
    CHECK_U64(back.consent.spent_today_sats, 51);
    CHECK_MEM(back.evm_addr, d.evm_addr, 40);
    CHECK_MEM(back.btc_pkh, d.btc_pkh, 40);
    CHECK_MEM(&back.btc_xpub, &d.btc_xpub, sizeof d.btc_xpub);
    CHECK_MEM(back.secp_blob, d.secp_blob, 128);
    CHECK_U64(back.consent.rollover_powered_seconds, 72000ull * 3 + 5);
    CHECK_EQ(back.consent.routers.n, 3);
    CHECK_MEM(&back.consent.routers, &d.consent.routers, sizeof d.consent.routers);
    CHECK_MEM(back.duress_hash, d.duress_hash, 32);
    CHECK_MEM(back.duress_salt, d.duress_salt, 16);
    CHECK_EQ(back.consent.duress_pin_set, 0);
    fill(&d, 4);
    n = storage_encode(&d, 43, buf, sizeof buf);
    CHECK(storage_decode(buf, sizeof buf, &back, &seq));
    CHECK_EQ(back.consent.duress_pin_set, 1);
    fill(&d, 3);
    n = storage_encode(&d, 42, buf, sizeof buf);
    CHECK(n <= STORAGE_RECORD_SIZE && n >= 1100);
    /* records from firmware versions 2 to 5 (no PIN / chain / router / duress fields) are not accepted as-is */
    {
        uint8_t v2[STORAGE_RECORD_SIZE];
        memcpy(v2, buf, sizeof v2);
        v2[4] = 2; v2[5] = 0;
        CHECK(!storage_decode(v2, sizeof v2, &back, &seq));
        v2[4] = 3;
        CHECK(!storage_decode(v2, sizeof v2, &back, &seq));
        v2[4] = 4;
        CHECK(!storage_decode(v2, sizeof v2, &back, &seq));
        v2[4] = 5;
        CHECK(!storage_decode(v2, sizeof v2, &back, &seq));
    }
    /* flip one byte -> CRC rejects */
    buf[20] ^= 0x01;
    CHECK(!storage_decode(buf, sizeof buf, &back, &seq));
    buf[20] ^= 0x01;
    CHECK(storage_decode(buf, sizeof buf, &back, &seq));
    /* corrupted CRC itself */
    buf[n - 1] ^= 0x80;
    CHECK(!storage_decode(buf, sizeof buf, &back, &seq));
    CHECK(storage_encode(&d, 1, buf, 100) == 0);
}

static void test_load_empty(void)
{
    storage_t s;
    storage_data_t d;
    memset(g_flash, 0xff, sizeof g_flash);
    storage_init(&s, &FL);
    CHECK(!storage_load(&s, &d));
    CHECK(!d.consent.initialized);
    CHECK_EQ(s.newest_slot, -1);
}

static void test_save_load_alternating(void)
{
    storage_t s;
    storage_data_t d, back;
    int i;
    memset(g_flash, 0xff, sizeof g_flash);
    g_erases = g_programs = 0;
    storage_init(&s, &FL);
    for (i = 1; i <= 5; i++) {
        fill(&d, i);
        CHECK_EQ(storage_save(&s, &d), 0);
        CHECK_EQ(s.newest_slot, (i - 1) % 2);
        CHECK_EQ(s.seq, i);
    }
    CHECK_EQ(g_erases, 5);
    /* fresh boot: newest (seq 5, slot 0) wins over seq 4 in slot 1 */
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK_EQ(s.seq, 5);
    CHECK_EQ(s.newest_slot, 0);
    CHECK_EQ(back.consent.day, 20395u + 5);
    CHECK_U64(back.consent.daily_limit, 2000000005ull);
}

static void test_pin_fields_persist(void)
{
    storage_t s;
    storage_data_t d, back;
    memset(g_flash, 0xff, sizeof g_flash);
    storage_init(&s, &FL);
    fill(&d, 2); /* pin_set, 2 failures */
    CHECK_EQ(storage_save(&s, &d), 0);
    d.consent.pin_failures = 4;
    CHECK_EQ(storage_save(&s, &d), 0);
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK(back.consent.pin_set);
    CHECK_EQ(back.consent.pin_failures, 4);
    /* a wipe writes an all-zero record that loads as "fresh device" */
    memset(&d, 0, sizeof d);
    CHECK_EQ(storage_save(&s, &d), 0);
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK(!back.consent.initialized && !back.consent.pin_set && back.consent.pin_failures == 0);
    {
        uint8_t zero[32] = {0};
        CHECK_MEM(back.pin_hash, zero, 32);
        CHECK_MEM(back.signer_blob[0], zero, 32);
        CHECK_MEM(back.secp_blob[0][0], zero, 32);
        CHECK_MEM(back.evm_addr[0], zero, 20);
        CHECK(u256_is_zero(&back.consent.daily_limit_wei));
        CHECK_EQ(back.consent.routers.n, 0);
        CHECK_MEM(back.duress_hash, zero, 32);
        CHECK(!back.consent.duress_pin_set);
    }
}

/* An owner-added router survives a reboot; the list can be emptied again. */
static void test_routers_round_trip(void)
{
    storage_t s;
    storage_data_t d, back;
    uint8_t thor[20];
    memset(thor, 0x7c, 20);
    memset(g_flash, 0xff, sizeof g_flash);
    storage_init(&s, &FL);
    memset(&d, 0, sizeof d);
    d.consent.initialized = true;
    CHECK_EQ(routers_add(&d.consent.routers, ROUTER_CHAIN_EVM, thor), 0);
    CHECK_EQ(storage_save(&s, &d), 0);
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK_EQ(back.consent.routers.n, 1);
    CHECK(routers_is_allowed(&back.consent.routers, ROUTER_CHAIN_EVM, thor));
    CHECK(!routers_is_allowed(&back.consent.routers, ROUTER_CHAIN_SOL, thor));
    CHECK(routers_remove(&back.consent.routers, ROUTER_CHAIN_EVM, thor));
    CHECK_EQ(storage_save(&s, &back), 0);
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK_EQ(back.consent.routers.n, 0);
    CHECK(!routers_is_allowed(&back.consent.routers, ROUTER_CHAIN_EVM, thor));
}

static void test_power_loss_keeps_previous(void)
{
    storage_t s;
    storage_data_t d, back;
    memset(g_flash, 0xff, sizeof g_flash);
    storage_init(&s, &FL);
    fill(&d, 1);
    CHECK_EQ(storage_save(&s, &d), 0);
    fill(&d, 2);
    g_fail_program = 1;
    CHECK(storage_save(&s, &d) != 0);
    g_fail_program = 0;
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK_EQ(back.consent.day, 20395u + 1); /* the torn record is ignored, previous one survives */
    CHECK_EQ(s.seq, 1);
    /* and the next save goes to the torn slot and succeeds */
    fill(&d, 3);
    CHECK_EQ(storage_save(&s, &d), 0);
    storage_init(&s, &FL);
    CHECK(storage_load(&s, &back));
    CHECK_EQ(back.consent.day, 20395u + 3);
}

int main(void)
{
    RUN(test_encode_decode);
    RUN(test_load_empty);
    RUN(test_save_load_alternating);
    RUN(test_pin_fields_persist);
    RUN(test_power_loss_keeps_previous);
    RUN(test_routers_round_trip);
    TEST_END();
}
