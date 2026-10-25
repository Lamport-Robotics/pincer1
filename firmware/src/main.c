/* Pincer1 consent controller - firmware entry point (Raspberry Pi Pico 2 / RP2350, Arm secure mode;
 * still builds for the RP2040 Pico without OTP secrets or secure boot).
 *
 * USB CDC loop: read newline-delimited JSON from the host (pincerd), dispatch to the pure-logic
 * modules, poll the touch pad every 5 ms (repeating timer sets a flag; the ISR does nothing else),
 * drive the on-board LED, persist state to the last two flash sectors (and the inheritance timer to the
 * two before them). The PIN tap rhythm is captured here from touch presses and verified by the signer
 * (inside the SE050).
 *
 * Trusted display (SSD1309 over SPI0): every screen is rendered here from the controller's OWN parse of a
 * request (trusted_ui.c); nothing the host sends as text ever reaches the panel. A sign request becomes
 * pending(touch) only after its first page has been rendered (consent_display_shown), and the seed words
 * after `init` are shown on this panel only, never sent over USB. */
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#if PICO_RP2350
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "pico/bootrom.h"
#endif
#if PICO_RP2040
#include "hardware/structs/vreg_and_chip_reset.h"
#elif PICO_RP2350
#include "hardware/structs/powman.h"
#endif

#include "bip32.h"
#include "btcpsbt.h"
#include "chainaddr.h"
#include "consent.h"
#include "display.h"
#include "eip712.h"
#include "evmtx.h"
#include "keygen.h"
#include "secp256k1.h"
#include "policy.h"
#include "proto.h"
#include "rng.h"
#include "signer.h"
#include "solmsg.h"
#include "storage.h"
#include "touch.h"
#include "trusted_ui.h"
#include "update.h"
#include "util.h"

#define PIN_TOUCH 15
#define PIN_LED PICO_DEFAULT_LED_PIN /* GP25 on Pico and Pico 2 */
#define PIN_BEACON 16                /* TRIG_CTRL to the locator beacon: high for 2 s on duress / tamper / wipe */
#define BEACON_PULSE_MS 2000
#define TICK_MS 5
/* trusted display: SSD1309 128x64, 4-wire SPI on SPI0 */
#define PIN_OLED_SCK 18
#define PIN_OLED_MOSI 19
#define PIN_OLED_CS 17
#define PIN_OLED_DC 20
#define PIN_OLED_RST 21
#define OLED_SPI spi0
#define OLED_SPI_HZ (8 * 1000 * 1000)
#define NOTICE_MS 4000      /* denied / signed / locked notices stay this long, then back to the idle screen */
#define NOTICE_LONG_MS 8000 /* router changes: long enough to compare the address */

/* ------------------------------------------------------------------ state */
static consent_t g_consent;
static storage_t g_storage;
static storage_data_t g_data;
static storage_t g_inh_storage;
static storage_inh_data_t g_inh; /* timer + signed inheritance tx; its region survives the wipe */
static proto_framer_t g_framer;
static touch_t g_touch;
static touch_rhythm_capture_t g_rhythm;
static proto_req_t g_req; /* ~2.5 KB, kept static */
static volatile uint32_t g_ticks;
static bool g_signer_ok;
static bool g_boot_tamper;   /* report once, on the first hello */
static uint32_t g_beacon_until; /* ms; 0 = line idle */

/* One-way trigger to the beacon board (beacon/). The beacon has its own battery and radios, so this
 * line is the only thing a thief cannot stop by cutting power to the main computer. */
static void beacon_trigger(void)
{
    gpio_put(PIN_BEACON, 1);
    g_beacon_until = to_ms_since_boot(get_absolute_time()) + BEACON_PULSE_MS;
}
static void beacon_service(uint32_t now)
{
    if (g_beacon_until && (int32_t)(now - g_beacon_until) >= 0) { gpio_put(PIN_BEACON, 0); g_beacon_until = 0; }
}
static bool g_usb_was_up;
static bool g_usb_ever_up;

/* ---- trusted display state ---- */
static display_t g_display;
static tui_doc_t g_doc;            /* the document of the pending sign / inheritance_set request */
static tui_foot_t g_doc_foot;
static bool g_doc_cancelable;
static uint32_t g_notice_until;    /* ms; 0 = no notice showing */
/* The 24 words of each set live here between `init` and the end of the word pages, then are zeroised. */
static char g_words_real[KEYGEN_MNEMONIC_CAP];
static char g_words_decoy[KEYGEN_MNEMONIC_CAP];
static char g_init_id[CONSENT_ID_MAX];
/* Init flow step 3: the real rhythm set_pin just created, so set_duress_pin right after it needs no second
 * entry of it. Valid only until the next consent request or REAL_PIN_HOLD_MS. */
static uint8_t g_real_pin[CONSENT_PIN_CODE_LEN];
static bool g_real_pin_valid;
static uint32_t g_real_pin_until;
#define REAL_PIN_HOLD_MS CONSENT_INIT_WORDS_TTL_MS

static void forget_real_pin(void)
{
    secure_zero(g_real_pin, sizeof g_real_pin);
    g_real_pin_valid = false;
}

/* Bytes of the request currently waiting for a touch (only one request can be pending). */
static struct {
    bool valid;
    proto_sign_kind_t kind;
    uint8_t bytes[PROTO_MAX_BYTES];
    uint16_t len;
    bool has_parsed;
    solmsg_summary_t parsed; /* Solana */
    evmtx_t evm;             /* evm_tx: re-parsed from `bytes` at signing time (it holds pointers) */
    uint8_t digest[32];      /* evm_msg / btc_msg: what gets signed */
    btcpsbt_t btc;           /* btc_psbt */
} g_pending_sign;
static uint8_t g_psbt_out[BTCPSBT_MAX_OUT_BYTES]; /* PSBT with partial signatures, built at signing time */

static char g_out[PROTO_LINE_MAX];

/* ------------------------------------------------------------------ helpers */
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void send_line(int n)
{
    if (n <= 0) return;
    stdio_put_string(g_out, n, false, false);
    stdio_flush();
}

static void send_denied(const char *id, const char *reason) { send_line(proto_fmt_denied(g_out, sizeof g_out, id, reason)); }
static void send_event(const char *ev, const char *what) { send_line(proto_fmt_event(g_out, sizeof g_out, ev, what)); }

/* ---- flash backend for storage.c: the last two sectors of whatever flash the board has
 * (PICO_FLASH_SIZE_BYTES: 4 MB on the Pico 2, 2 MB on the Pico) hold the main record; the two sectors
 * before them hold the inheritance record. Offsets handed to the ops are region-relative. ---- */
#define STORAGE_BASE (PICO_FLASH_SIZE_BYTES - STORAGE_REGION_SIZE)
#define STORAGE_INH_BASE (STORAGE_BASE - STORAGE_INH_REGION_SIZE)

static int region_read(uint32_t base, uint32_t size, uint32_t off, void *buf, size_t len)
{
    if (off + len > size) return -1;
    memcpy(buf, (const void *)(XIP_BASE + base + off), len);
    return 0;
}
static int region_erase(uint32_t base, uint32_t size, uint32_t off)
{
    uint32_t irq;
    if (off % FLASH_SECTOR_SIZE || off + FLASH_SECTOR_SIZE > size) return -1;
    irq = save_and_disable_interrupts();
    flash_range_erase(base + off, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
    return 0;
}
static int region_program(uint32_t base, uint32_t size, uint32_t off, const void *buf, size_t len)
{
    uint32_t irq;
    if (off % FLASH_PAGE_SIZE || len % FLASH_PAGE_SIZE || off + len > size) return -1;
    irq = save_and_disable_interrupts();
    flash_range_program(base + off, buf, len);
    restore_interrupts(irq);
    return 0;
}
static int fl_read(uint32_t off, void *buf, size_t len) { return region_read(STORAGE_BASE, STORAGE_REGION_SIZE, off, buf, len); }
static int fl_erase(uint32_t off) { return region_erase(STORAGE_BASE, STORAGE_REGION_SIZE, off); }
static int fl_program(uint32_t off, const void *buf, size_t len) { return region_program(STORAGE_BASE, STORAGE_REGION_SIZE, off, buf, len); }
static int fi_read(uint32_t off, void *buf, size_t len) { return region_read(STORAGE_INH_BASE, STORAGE_INH_REGION_SIZE, off, buf, len); }
static int fi_erase(uint32_t off) { return region_erase(STORAGE_INH_BASE, STORAGE_INH_REGION_SIZE, off); }
static int fi_program(uint32_t off, const void *buf, size_t len) { return region_program(STORAGE_INH_BASE, STORAGE_INH_REGION_SIZE, off, buf, len); }
static const storage_flash_t g_flash_ops = {fl_read, fl_erase, fl_program};
static const storage_flash_t g_inh_flash_ops = {fi_read, fi_erase, fi_program};

/* ---- firmware update: the spare A/B partition and the trial ledger -------------------------------
 * The ledger is one sector below the inheritance region. The spare partition is found through the
 * bootrom's partition table on the RP2350; without a table (bench boards, RP2040) updates are refused. */
#define LEDGER_BASE (STORAGE_INH_BASE - FLASH_SECTOR_SIZE)
static int lg_read(uint32_t off, void *buf, size_t len) { return region_read(LEDGER_BASE, FLASH_SECTOR_SIZE, off, buf, len); }
static int lg_erase(uint32_t off) { return region_erase(LEDGER_BASE, FLASH_SECTOR_SIZE, off); }
static int lg_program(uint32_t off, const void *buf, size_t len) { return region_program(LEDGER_BASE, FLASH_SECTOR_SIZE, off, buf, len); }
static const update_flash_t g_ledger_fl = {FLASH_SECTOR_SIZE, lg_erase, lg_program, lg_read};

static int8_t g_boot_partition = -1;  /* partition this image runs from */
static uint32_t g_my_base;            /* its flash offset */
static int8_t g_spare_partition = -1; /* the other half of the A/B pair */
static uint32_t g_spare_base, g_spare_size;
static bool g_trial;                  /* this image booted under an armed ledger */
static int up_read(uint32_t off, void *buf, size_t len) { return region_read(g_spare_base, g_spare_size, off, buf, len); }
static int up_erase(uint32_t off) { return region_erase(g_spare_base, g_spare_size, off); }
static int up_program(uint32_t off, const void *buf, size_t len) { return region_program(g_spare_base, g_spare_size, off, buf, len); }
static update_flash_t g_update_fl = {0, up_erase, up_program, up_read};
static update_t g_update;

#if PICO_RP2350
/* Reads the resident partition table: words[0] echoes the flags, words[1] is the PT info (partition count
 * in the low byte), then two words for the unpartitioned space, then two per partition: location
 * (first/last sector) and flags (link type/value). Finds the partition we booted from and its A/B twin. */
static void find_ab_partitions(void)
{
    static uint32_t words[2 + 2 + 2 * PARTITION_TABLE_MAX_PARTITIONS];
    boot_info_t bi;
    int n, count, i;
    uint32_t my_flags;
    if (!rom_get_boot_info(&bi) || bi.partition < 0 || bi.partition >= PARTITION_TABLE_MAX_PARTITIONS) return;
    n = rom_get_partition_table_info(words, sizeof words / sizeof words[0], PT_INFO_PT_INFO | PT_INFO_PARTITION_LOCATION_AND_FLAGS);
    if (n < 4) return;
    count = (int)(words[1] & 0xff);
    if (bi.partition >= count) return;
    g_boot_partition = (int8_t)bi.partition;
    g_my_base = ((words[4 + 2 * bi.partition] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) * FLASH_SECTOR_SIZE;
    my_flags = words[4 + 2 * bi.partition + 1];
    if (((my_flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION) {
        /* we are a B partition: our twin is the A we link to */
        g_spare_partition = (int8_t)((my_flags & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB);
    } else {
        for (i = 0; i < count; i++) {
            uint32_t fl = words[4 + 2 * i + 1];
            if (i != bi.partition &&
                ((fl & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION &&
                (int)((fl & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB) == bi.partition) {
                g_spare_partition = (int8_t)i;
                break;
            }
        }
    }
    if (g_spare_partition < 0 || g_spare_partition >= count) { g_spare_partition = -1; return; }
    {
        uint32_t loc = words[4 + 2 * g_spare_partition];
        uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        uint32_t last = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
        g_spare_base = first * FLASH_SECTOR_SIZE;
        g_spare_size = (last - first + 1) * FLASH_SECTOR_SIZE;
        g_update_fl.size = g_spare_size;
    }
    g_trial = bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING;
}
#endif

/* Erases our own first sector (vector table + IMAGE_DEF) so the bootrom no longer sees this image, then
 * lets the watchdog armed by the caller reboot the chip. Runs from RAM: once the sector is gone nothing in
 * this partition's first 4 KiB may be executed, and the erase itself takes tens of milliseconds. */
static void __no_inline_not_in_flash_func(self_invalidate)(uint32_t my_base)
{
    watchdog_reboot(0, 0, 800); /* fires after the erase (sector erase ≤ 400 ms), from flash code, before the erase */
    save_and_disable_interrupts();
    flash_range_erase(my_base, FLASH_SECTOR_SIZE);
    while (1) tight_loop_contents();
}

/* On every boot: an armed ledger for OUR partition means we are the image on trial. Count the boot; past
 * the limit the daemon never confirmed us, so we erase our own first sector (the IMAGE_DEF lives there)
 * and reboot: the bootrom then finds only the previous image and boots that. */
static void update_boot_check(void)
{
    update_ledger_t l;
#if PICO_RP2350
    find_ab_partitions();
#endif
    update_init(&g_update, &g_update_fl);
    update_ledger_read(&g_ledger_fl, &l);
    if (!l.armed || g_boot_partition < 0 || l.partition != (uint8_t)g_boot_partition) return;
    g_trial = true;
    if (update_ledger_boot(&g_ledger_fl) <= UPDATE_TRIAL_BOOTS) return;
    /* trial over, never committed: step aside */
    update_ledger_clear(&g_ledger_fl);
    self_invalidate(g_my_base);
}

static void handle_update(void)
{
    update_rc_t rc;
    consent_result_t res;
    consent_reason_t why;
    if (g_spare_partition < 0 && g_req.type != PROTO_REQ_UPDATE_COMMIT && g_req.type != PROTO_REQ_UPDATE_ABORT) {
        send_line(proto_fmt_denied(g_out, sizeof g_out, g_req.id, "not_supported"));
        return;
    }
    switch (g_req.type) {
    case PROTO_REQ_UPDATE_BEGIN:
        if (g_update.state == UPDATE_VERIFIED) update_abort(&g_update); /* a fresh begin replaces a staged image */
        rc = update_begin(&g_update, g_req.update.size, g_req.update.sha256, g_req.update.version);
        if (rc != UPDATE_OK) { send_line(proto_fmt_denied(g_out, sizeof g_out, g_req.id, update_rc_str(rc))); return; }
        send_line(proto_fmt_update_begin(g_out, sizeof g_out, g_req.id, UPDATE_CHUNK_MAX, g_spare_size));
        return;
    case PROTO_REQ_UPDATE_CHUNK:
        rc = update_chunk(&g_update, g_req.update.off, g_req.update.data, g_req.update.len);
        if (rc != UPDATE_OK) { send_line(proto_fmt_denied(g_out, sizeof g_out, g_req.id, update_rc_str(rc))); return; }
        send_line(proto_fmt_ok(g_out, sizeof g_out, "update_chunk", g_req.id));
        return;
    case PROTO_REQ_UPDATE_END:
        rc = update_end(&g_update);
        if (rc != UPDATE_OK) { send_line(proto_fmt_denied(g_out, sizeof g_out, g_req.id, update_rc_str(rc))); return; }
        send_line(proto_fmt_ok(g_out, sizeof g_out, "update_end", g_req.id));
        return;
    case PROTO_REQ_UPDATE_APPLY:
        if (g_update.state != UPDATE_VERIFIED) { send_line(proto_fmt_denied(g_out, sizeof g_out, g_req.id, "state")); return; }
        forget_real_pin();
        res = consent_begin_update_apply(&g_consent, g_req.id, g_update.version, g_update.expect, now_ms(), &why);
        if (res == CONSENT_R_PENDING) {
            char line[32];
            snprintf(line, sizeof line, "v%s  %02x%02x%02x%02x", g_update.version, g_update.expect[0], g_update.expect[1], g_update.expect[2], g_update.expect[3]);
            ui_notice("FIRMWARE", line, "hold 3s to install", NOTICE_LONG_MS);
        }
        reply_pending_or_denied(res, g_req.id, why);
        return;
    case PROTO_REQ_UPDATE_COMMIT: {
        bool was_trial = g_trial, bought = false;
#if PICO_RP2350
        boot_info_t bi;
        if (rom_get_boot_info(&bi) && (bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING)) {
            static uint8_t buy_buf[4096] __attribute__((aligned(4)));
            bought = rom_explicit_buy(buy_buf, sizeof buy_buf) == 0;
        }
#endif
        if (g_trial) { update_ledger_clear(&g_ledger_fl); g_trial = false; }
        send_line(proto_fmt_update_commit(g_out, sizeof g_out, g_req.id, was_trial, bought));
        if (was_trial) ui_notice("FIRMWARE", "confirmed", PROTO_FW_VERSION, NOTICE_MS);
        return;
    }
    case PROTO_REQ_UPDATE_ABORT:
        update_abort(&g_update);
        send_line(proto_fmt_ok(g_out, sizeof g_out, "update_abort", g_req.id));
        return;
    default:
        return;
    }
}

static void do_update_apply(const consent_action_t *a)
{
    if (g_update.state != UPDATE_VERIFIED || g_spare_partition < 0) {
        send_line(proto_fmt_denied(g_out, sizeof g_out, a->id, "state"));
        return;
    }
    if (update_ledger_arm(&g_ledger_fl, (uint8_t)g_spare_partition, g_update.version) != 0) {
        send_line(proto_fmt_denied(g_out, sizeof g_out, a->id, "flash"));
        return;
    }
    send_line(proto_fmt_ok(g_out, sizeof g_out, "update_apply", a->id));
    ui_notice("FIRMWARE", "rebooting", a->update_version, NOTICE_MS);
    persist();
    stdio_flush();
    sleep_ms(200);
#if PICO_RP2350
    /* a flash-update boot of the spare partition: the bootrom checks the signature against OTP first */
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 50, XIP_BASE + g_spare_base, 0);
#endif
    watchdog_reboot(0, 0, 50);
    while (1) tight_loop_contents();
}

/* Timer + inheritance record. Written hourly (consent_tick raises ih_dirty), on inheritance changes, and
 * whenever the main record is written anyway. Never written with zeros by the wipe. */
static void persist_inh(void)
{
    g_inh.ih = g_consent.ih;
    if (storage_inh_save(&g_inh_storage, &g_inh) == 0) consent_inheritance_persisted(&g_consent);
}

static void persist(void)
{
    if (g_consent.dirty) {
        g_data.consent = g_consent.p;
        signer_soft_export(g_data.signer_blob, g_data.pin_hash); /* zeros with the SE050 */
        signer_soft_export_secp(g_data.secp_blob);
        signer_soft_export_duress(g_data.duress_hash, g_data.duress_salt);
        if (storage_save(&g_storage, &g_data) == 0) g_consent.dirty = false;
        /* the uptime counter rides along with every state change that writes flash anyway */
        if (g_consent.ih.powered_seconds != g_consent.ih_persisted_seconds) g_consent.ih_dirty = true;
    }
    if (g_consent.ih_dirty) persist_inh();
}

static void clear_pending_sign(void)
{
    secure_zero(&g_pending_sign, sizeof g_pending_sign);
}

static const uint8_t *owner_pubkey(void)
{
    static const uint8_t zero[32] = {0};
    const uint8_t *k = g_data.pubkey[consent_active_slot(&g_consent)];
    return memcmp(k, zero, 32) == 0 ? NULL : k;
}

/* Active slot's EVM address / Bitcoin key hash (NULL until init derived them). */
static const uint8_t *owner_evm_addr(void)
{
    static const uint8_t zero[20] = {0};
    const uint8_t *a = g_data.evm_addr[consent_active_slot(&g_consent)];
    return memcmp(a, zero, 20) == 0 ? NULL : a;
}
static const uint8_t *owner_btc_pkh(void)
{
    static const uint8_t zero[20] = {0};
    const uint8_t *a = g_data.btc_pkh[consent_active_slot(&g_consent)];
    return memcmp(a, zero, 20) == 0 ? NULL : a;
}

/* Compressed public key of the active slot's Bitcoin key, from the signing back-end. */
static bool owner_btc_pub33(uint8_t out[33])
{
    uint8_t pub[64];
    if (signer_pubkey_secp256k1(consent_active_slot(&g_consent), SIGNER_CHAIN_BTC, pub) != 0) return false;
    secp256k1_compress(pub, out);
    return true;
}

/* Address cache in flash follows the back-end (the SE050 is the source of truth). */
static void refresh_addresses(uint8_t slot)
{
    uint8_t pub[64], a[20];
    if (signer_secp256k1_present(slot, SIGNER_CHAIN_EVM) && signer_pubkey_secp256k1(slot, SIGNER_CHAIN_EVM, pub) == 0) {
        evm_address_from_pubkey(pub, a);
        if (memcmp(a, g_data.evm_addr[slot], 20) != 0) { memcpy(g_data.evm_addr[slot], a, 20); g_consent.dirty = true; }
    }
    if (signer_secp256k1_present(slot, SIGNER_CHAIN_BTC) && signer_pubkey_secp256k1(slot, SIGNER_CHAIN_BTC, pub) == 0) {
        btc_pkh_from_pubkey(pub, a);
        if (memcmp(a, g_data.btc_pkh[slot], 20) != 0) { memcpy(g_data.btc_pkh[slot], a, 20); g_consent.dirty = true; }
    }
}

/* ------------------------------------------------------------------ trusted display */
#ifndef PINCER_NO_DISPLAY
static void oled_select(bool dc) { gpio_put(PIN_OLED_DC, dc); gpio_put(PIN_OLED_CS, 0); }
static void oled_release(void) { gpio_put(PIN_OLED_CS, 1); }
static void oled_cmd(const uint8_t *b, size_t n) { oled_select(false); spi_write_blocking(OLED_SPI, b, n); oled_release(); }
static void oled_data(const uint8_t *b, size_t n) { oled_select(true); spi_write_blocking(OLED_SPI, b, n); oled_release(); }
static void oled_reset(void)
{
    gpio_put(PIN_OLED_RST, 1); sleep_ms(1);
    gpio_put(PIN_OLED_RST, 0); sleep_ms(10);
    gpio_put(PIN_OLED_RST, 1); sleep_ms(10);
}
static const display_hw_t g_oled_hw = {oled_cmd, oled_data, oled_reset};

static void display_hw_init(void)
{
    spi_init(OLED_SPI, OLED_SPI_HZ);
    spi_set_format(OLED_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_OLED_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_OLED_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_OLED_CS); gpio_set_dir(PIN_OLED_CS, GPIO_OUT); gpio_put(PIN_OLED_CS, 1);
    gpio_init(PIN_OLED_DC); gpio_set_dir(PIN_OLED_DC, GPIO_OUT); gpio_put(PIN_OLED_DC, 0);
    gpio_init(PIN_OLED_RST); gpio_set_dir(PIN_OLED_RST, GPIO_OUT); gpio_put(PIN_OLED_RST, 1);
    display_init(&g_display, &g_oled_hw);
}
#else
/* Bench build without the panel: every screen is still rendered into the framebuffer and the consent machine
 * still goes through the "shown" step; nothing leaves the chip. hello reports display:false. */
static void display_hw_init(void) { display_init(&g_display, NULL); }
#endif

static void send_display_event(uint8_t page, uint8_t pages)
{
    send_line(proto_fmt_event_display(g_out, sizeof g_out, (uint8_t)(page + 1), pages));
}

static void ui_idle(void)
{
    tui_idle_t st;
    tui_page_t pg;
    memset(&st, 0, sizeof st);
    st.initialized = g_consent.p.initialized;
    st.locked = g_consent.locked;
    st.pin_set = g_consent.p.pin_set;
    if (st.initialized) { st.sol = owner_pubkey(); st.evm = owner_evm_addr(); st.btc_pkh = owner_btc_pkh(); } /* active slot: duress is invisible */
    tui_build_idle(&st, &pg);
    tui_render(&g_display, &pg, TUI_FOOT_NONE, 1, 1, false);
    display_flush(&g_display);
    g_notice_until = 0;
}

static void ui_notice(const char *head, const char *line1, const char *line2, uint32_t ms)
{
    tui_page_t pg;
    tui_build_notice(head, line1, line2, &pg);
    tui_render(&g_display, &pg, TUI_FOOT_NONE, 1, 1, false);
    display_flush(&g_display);
    g_notice_until = now_ms() + ms;
    if (g_notice_until == 0) g_notice_until = 1;
}

/* Page `page` of the pending sign / inheritance document: render, then tell consent it was shown. */
static void ui_sign_page(uint8_t page)
{
    tui_render_doc(&g_display, &g_doc, page, g_doc_foot, g_doc_cancelable);
    display_flush(&g_display);
    g_notice_until = 0;
    consent_display_shown(&g_consent, page, g_doc.n_pages);
    send_display_event(page, g_doc.n_pages);
}

static void ui_words_page(uint8_t page)
{
    tui_page_t pg;
    tui_foot_t foot = tui_build_words_page(page, g_words_real, g_words_decoy, &pg);
    tui_render(&g_display, &pg, foot, (uint8_t)(page + 1), TUI_WORDS_PAGES, true);
    display_flush(&g_display);
    g_notice_until = 0;
    consent_display_shown(&g_consent, page, TUI_WORDS_PAGES);
    send_display_event(page, TUI_WORDS_PAGES);
}

#define PROMPT_LINE 128 /* generous: tui cuts / wraps to the 21-cell line itself */
static void fmt_limit(char *out, size_t cap, const char *tag, uint64_t v, uint8_t decimals, const char *unit)
{
    char amt[32];
    tui_fmt_units(v, decimals, amt, sizeof amt);
    snprintf(out, cap, "%.12s %.24s %.8s", tag, amt, unit);
}

/* The screen for a pending request that is not a sign document: what the PIN rhythm / long touch will do. */
static void ui_pending_prompt(uint8_t taps)
{
    tui_page_t pg;
    char l1[PROMPT_LINE] = "", l2[PROMPT_LINE] = "";
    const consent_policy_req_t *pr = &g_consent.pending.policy;
    switch (g_consent.pending.op) {
    case CONSENT_OP_INIT:
        tui_build_prompt("INIT " "\xC2\xB7" " NEW WALLET", "creates 24 words +", "a decoy set on this screen", &pg);
        tui_render(&g_display, &pg, TUI_FOOT_HOLD, 1, 1, false);
        break;
    case CONSENT_OP_ELEVATE:
        if (g_consent.pending.chain == CONSENT_CHAIN_EVM) {
            char amt[U256_DEC_MAX + 2];
            tui_fmt_units_u256(&g_consent.pending.limit_wei, 18, amt, sizeof amt);
            snprintf(l1, sizeof l1, "EVM limit %.80s ETH", amt);
        } else if (g_consent.pending.chain == CONSENT_CHAIN_BTC) {
            fmt_limit(l1, sizeof l1, "BTC limit", g_consent.pending.limit, 8, "BTC");
        } else {
            fmt_limit(l1, sizeof l1, "SOL limit", g_consent.pending.limit, 9, "SOL");
        }
        tui_build_prompt("ELEVATE " "\xC2\xB7" " ONE REQUEST", l1, "for the next 2 minutes", &pg);
        tui_render(&g_display, &pg, TUI_FOOT_HOLD, 1, 1, false);
        break;
    case CONSENT_OP_ALLOWANCE:
        fmt_limit(l1, sizeof l1, "budget", g_consent.pending.allowance.lamports, 9, "SOL");
        fmt_limit(l2, sizeof l2, "per tx", g_consent.pending.allowance.max_per_tx, 9, "SOL");
        tui_build_prompt("ALLOWANCE " "\xC2\xB7" " NO-TOUCH", l1, l2, &pg);
        tui_render(&g_display, &pg, TUI_FOOT_HOLD, 1, 1, false);
        break;
    case CONSENT_OP_INHERITANCE_CLEAR:
        tui_build_prompt("INHERITANCE " "\xC2\xB7" " CLEAR", "removes the stored", "heir transaction", &pg);
        tui_render(&g_display, &pg, TUI_FOOT_HOLD, 1, 1, false);
        break;
    case CONSENT_OP_UNLOCK:
        if (g_consent.pending.need == CONSENT_NEED_PIN) tui_build_pin("UNLOCK", NULL, NULL, taps, false, &pg);
        else tui_build_prompt("UNLOCK", "touch to unlock", "(no PIN set yet)", &pg);
        tui_render(&g_display, &pg, g_consent.pending.need == CONSENT_NEED_PIN ? TUI_FOOT_PIN : TUI_FOOT_APPROVE, 1, 1, false);
        break;
    case CONSENT_OP_SET_PIN:
        tui_build_pin("SET PIN", "4-8 taps, short/long", "gaps, pause 1.5s", taps, g_consent.pending.pin_stage == 1, &pg);
        tui_render(&g_display, &pg, TUI_FOOT_PIN, 1, 1, false);
        break;
    case CONSENT_OP_SET_DURESS_PIN:
        if (g_consent.pending.pin_stage == 0) tui_build_pin("DURESS RHYTHM", "first: your real PIN", NULL, taps, false, &pg);
        else tui_build_pin("DURESS RHYTHM", "tap a second code", "differs from your PIN", taps, g_consent.pending.pin_stage == 2, &pg);
        tui_render(&g_display, &pg, TUI_FOOT_PIN, 1, 1, false);
        break;
    case CONSENT_OP_DECOY_ADDRESSES:
        tui_build_pin("DECOY ADDRESSES", "show the decoy", "wallet addresses", taps, false, &pg);
        tui_render(&g_display, &pg, TUI_FOOT_PIN, 1, 1, false);
        break;
    case CONSENT_OP_POLICY:
        if (pr->has_router_add || pr->has_router_remove) {
            char dots[24];
            size_t i, o = 0;
            tui_build_router(pr->has_router_add ? "ADD ROUTER" : "REMOVE ROUTER", &pr->router, &pg);
            for (i = 0; i < taps && i < 10; i++) { dots[o++] = '*'; dots[o++] = ' '; }
            dots[o] = 0;
            tui_page_add(&pg, TUI_TEXT, taps ? dots : "PIN: tap your rhythm"); /* one dot per tap, never the gaps */
        } else {
            if (pr->has_sol) fmt_limit(l1, sizeof l1, "SOL/day", pr->sol, 9, "SOL");
            if (pr->has_sats) fmt_limit(pr->has_sol ? l2 : l1, PROMPT_LINE, "BTC/day", pr->sats, 8, "BTC");
            if (pr->has_wei) {
                char amt[U256_DEC_MAX + 2];
                tui_fmt_units_u256(&pr->wei, 18, amt, sizeof amt);
                snprintf(l1[0] ? l2 : l1, PROMPT_LINE, "EVM/day %.80s ETH", amt);
            }
            tui_build_pin("POLICY", l1[0] ? l1 : NULL, l2[0] ? l2 : NULL, taps, false, &pg);
        }
        tui_render(&g_display, &pg, TUI_FOOT_PIN, 1, 1, false);
        break;
    default:
        return;
    }
    display_flush(&g_display);
    g_notice_until = 0;
}

/* Back to whatever the state calls for: the pending request's screen, or idle. */
static void ui_refresh(void)
{
    if (g_consent.pending.op == CONSENT_OP_NONE) { ui_idle(); return; }
    if (g_consent.pending.op == CONSENT_OP_INIT_WORDS) { ui_words_page(consent_pending_page(&g_consent)); return; }
    if (consent_pending_display_gated(&g_consent)) { ui_sign_page(consent_pending_page(&g_consent)); return; }
    ui_pending_prompt(g_rhythm.active ? g_rhythm.n_taps : 0);
}

static void send_denied_shown(const char *id, const char *reason)
{
    send_denied(id, reason);
    ui_notice("DENIED", reason, NULL, NOTICE_MS);
}

/* ------------------------------------------------------------------ actions */
/* Signs the pending request with the key the action names. Returns true when a reply was sent and the spend
 * must be committed. */
static bool sign_pending(const consent_action_t *a)
{
    uint8_t sig[64], sig65[65], digest[32];
    bool ok = false;
    switch (g_pending_sign.kind) {
    case PROTO_KIND_TX:
    case PROTO_KIND_MSG:
        if (signer_sign(a->slot, g_pending_sign.bytes, g_pending_sign.len, sig) != 0) break;
        send_line(proto_fmt_signed(g_out, sizeof g_out, a->id, sig));
        ok = true;
        break;
    case PROTO_KIND_EVM_TX: {
        evmtx_t tx;
        if (evmtx_parse(g_pending_sign.bytes, g_pending_sign.len, &tx) != EVMTX_OK) break;
        evmtx_signing_digest(g_pending_sign.bytes, g_pending_sign.len, digest);
        if (signer_sign_secp256k1(a->slot, SIGNER_CHAIN_EVM, digest, sig65) != 0) break;
        evmtx_encode_signature(&tx, sig65, sig65[64], sig65);
        send_line(proto_fmt_signed_bytes(g_out, sizeof g_out, a->id, sig65, 65));
        ok = true;
        break;
    }
    case PROTO_KIND_EVM_MSG:
        if (signer_sign_secp256k1(a->slot, SIGNER_CHAIN_EVM, g_pending_sign.digest, sig65) != 0) break;
        sig65[64] = (uint8_t)(27 + (sig65[64] & 1)); /* personal_sign / EIP-712: v = 27 + recid */
        send_line(proto_fmt_signed_bytes(g_out, sizeof g_out, a->id, sig65, 65));
        ok = true;
        break;
    case PROTO_KIND_BTC_PSBT: {
        static uint8_t sigs[BTCPSBT_MAX_INPUTS][64];
        const btcpsbt_t *p = &g_pending_sign.btc;
        uint8_t i;
        size_t n;
        bool all = true;
        for (i = 0; i < p->n_in && all; i++) {
            if (!p->in[i].is_owner) continue;
            all = btcpsbt_sighash(p, g_pending_sign.bytes, i, digest) &&
                  signer_sign_secp256k1(a->slot, SIGNER_CHAIN_BTC, digest, sig65) == 0;
            if (all) memcpy(sigs[i], sig65, 64);
        }
        if (!all) { secure_zero(sigs, sizeof sigs); break; }
        n = btcpsbt_insert_sigs(p, g_pending_sign.bytes, g_pending_sign.len, (const uint8_t (*)[64])sigs, g_psbt_out, sizeof g_psbt_out);
        secure_zero(sigs, sizeof sigs);
        if (!n) break;
        send_line(proto_fmt_signed_psbt(g_out, sizeof g_out, a->id, g_psbt_out, n));
        secure_zero(g_psbt_out, n);
        ok = true;
        break;
    }
    case PROTO_KIND_BTC_MSG: {
        uint8_t pub33[33], wit[BTCPSBT_PARTIAL_SIG_MAX];
        size_t n;
        if (!owner_btc_pub33(pub33)) break;
        if (signer_sign_secp256k1(a->slot, SIGNER_CHAIN_BTC, g_pending_sign.digest, sig65) != 0) break;
        n = btcpsbt_bip322_witness(sig65, pub33, wit, sizeof wit);
        if (!n) break;
        send_line(proto_fmt_signed_bytes(g_out, sizeof g_out, a->id, wit, n));
        ok = true;
        break;
    }
    default:
        break;
    }
    secure_zero(sig, sizeof sig);
    secure_zero(sig65, sizeof sig65);
    secure_zero(digest, sizeof digest);
    return ok;
}

static void do_sign_now(const consent_action_t *a)
{
    if (!g_signer_ok || !g_pending_sign.valid || !sign_pending(a)) {
        send_denied_shown(a->id, "policy");
    } else {
        consent_commit_spend(&g_consent, a);
        persist();
        ui_notice("SIGNED", a->via_allowance ? "allowance" : "by touch", NULL, NOTICE_MS);
    }
    clear_pending_sign();
}

/* Undo a half-done init: destroy the imported keys and the words. */
static void abort_init(const char *reason)
{
    if (g_signer_ok) signer_wipe();
    signer_pin_close();
    memset(g_data.pubkey, 0, sizeof g_data.pubkey);
    memset(g_data.evm_addr, 0, sizeof g_data.evm_addr);
    memset(g_data.btc_pkh, 0, sizeof g_data.btc_pkh);
    memset(g_data.btc_xpub, 0, sizeof g_data.btc_xpub);
    secure_zero(g_words_real, sizeof g_words_real);
    secure_zero(g_words_decoy, sizeof g_words_decoy);
    g_consent.dirty = true;
    persist();
    send_denied_shown(g_init_id, reason);
    secure_zero(g_init_id, sizeof g_init_id);
}

/* Long touch arrived for `init`: derive both key sets, import them, then show the words on the trusted display.
 * Nothing but `pending` goes to the host until the owner has tapped through every page. */
static void do_init(const consent_action_t *a)
{
    uint8_t entropy[KEYGEN_ENTROPY_LEN], pub[64];
    keygen_keys_t keys;
    uint8_t slot;
    consent_reason_t why = CONSENT_DENY_NONE;
    bool ok = g_signer_ok;
    for (slot = 0; ok && slot < SIGNER_SLOTS; slot++) {
        char *mnemonic = slot == 0 ? g_words_real : g_words_decoy;
        /* Real key (slot 0) and decoy (slot 1): independent entropy, same derivation paths for all chains. */
        rng_fill(entropy, sizeof entropy);
        if (keygen_derive_all(entropy, mnemonic, KEYGEN_MNEMONIC_CAP, &keys) != 0 ||
            signer_import(slot, keys.sol_priv, g_data.pubkey[slot]) != 0)
            ok = false;
        if (ok && signer_import_secp256k1(slot, SIGNER_CHAIN_EVM, keys.evm_priv, pub) == 0) evm_address_from_pubkey(pub, g_data.evm_addr[slot]);
        else ok = false;
        if (ok && signer_import_secp256k1(slot, SIGNER_CHAIN_BTC, keys.btc_priv, pub) == 0) btc_pkh_from_pubkey(pub, g_data.btc_pkh[slot]);
        else ok = false;
        g_data.btc_xpub[slot] = keys.btc_account; /* public: lets the controller recognise change outputs */
        secure_zero(entropy, sizeof entropy);
        secure_zero(&keys, sizeof keys);
        secure_zero(pub, sizeof pub);
    }
    memcpy(g_init_id, a->id, sizeof g_init_id);
    if (!ok || consent_begin_init_words(&g_consent, a->id, TUI_WORDS_PAGES, now_ms(), &why) != CONSENT_R_PENDING) {
        abort_init("policy");
        return;
    }
    ui_words_page(0);
    send_line(proto_fmt_pending(g_out, sizeof g_out, a->id, "touch", NULL));
}

/* The owner tapped through the last word page: now the device is initialised and the host learns the
 * public keys. The words are gone from RAM after this. */
static void finish_init(const consent_action_t *a)
{
    secure_zero(g_words_real, sizeof g_words_real);
    secure_zero(g_words_decoy, sizeof g_words_decoy);
    consent_mark_initialized(&g_consent);
    persist();
    {
        proto_addrs_t real = {g_data.pubkey[0], g_data.evm_addr[0], g_data.btc_pkh[0]};
        proto_addrs_t decoy = {g_data.pubkey[1], g_data.evm_addr[1], g_data.btc_pkh[1]};
        send_line(proto_fmt_init(g_out, sizeof g_out, a->id, &real, &decoy)); /* decoy keys: here, once, never in hello */
    }
    secure_zero(g_init_id, sizeof g_init_id);
    ui_notice("WALLET CREATED", "set the PIN", "from the setup app", NOTICE_LONG_MS);
}

static void do_set_pin(const consent_action_t *a)
{
    if (!g_signer_ok || signer_pin_set(a->pin) != 0) {
        send_denied_shown(a->id, "policy");
        return;
    }
    consent_mark_pin_set(&g_consent);
    memcpy(g_real_pin, a->pin, CONSENT_PIN_CODE_LEN); /* step 3 (set_duress_pin) may follow without re-entering it */
    g_real_pin_valid = true;
    g_real_pin_until = now_ms() + REAL_PIN_HOLD_MS;
    send_line(proto_fmt_ok(g_out, sizeof g_out, "set_pin", a->id));
    persist();
    ui_notice("PIN SET", "ready", "duress rhythm next?", NOTICE_MS);
}

static void do_set_duress_pin(const consent_action_t *a)
{
    uint8_t salt[16];
    int rc;
    rng_fill(salt, sizeof salt);
    rc = g_signer_ok ? signer_duress_pin_set(a->pin, salt) : -1;
    secure_zero(salt, sizeof salt);
    if (rc != 0) {
        send_denied_shown(a->id, "policy");
        return;
    }
    consent_mark_duress_pin_set(&g_consent);
    send_line(proto_fmt_ok(g_out, sizeof g_out, "set_duress_pin", a->id));
    persist();
    ui_notice("DURESS RHYTHM", "set", "unlocks the decoy", NOTICE_MS);
}

/* PIN verified: the decoy slot's public keys, so the owner can fund it. */
static void do_decoy_addresses(const consent_action_t *a)
{
    proto_addrs_t d;
    tui_idle_t st;
    tui_page_t pg;
    d.pubkey = g_data.pubkey[CONSENT_SLOT_DECOY];
    d.evm_addr = g_data.evm_addr[CONSENT_SLOT_DECOY];
    d.btc_pkh = g_data.btc_pkh[CONSENT_SLOT_DECOY];
    send_line(proto_fmt_decoy_addresses(g_out, sizeof g_out, a->id, &d));
    memset(&st, 0, sizeof st);
    st.initialized = true; st.pin_set = true;
    st.sol = d.pubkey; st.evm = d.evm_addr; st.btc_pkh = d.btc_pkh;
    tui_build_idle(&st, &pg);
    snprintf(pg.l[0].text, sizeof pg.l[0].text, "DECOY " "\xC2\xB7" " ADDRESSES");
    tui_render(&g_display, &pg, TUI_FOOT_NONE, 1, 1, false);
    display_flush(&g_display);
    g_notice_until = now_ms() + NOTICE_LONG_MS;
}

/* Long touch (or touch, when replacing) arrived for inheritance_set: sign once with the real key and keep
 * message + signature in the inheritance region. The bytes never go back to the host now. */
static void do_inheritance_set(const consent_action_t *a)
{
    uint8_t sig[64];
    if (!g_signer_ok || !g_pending_sign.valid || g_pending_sign.len > CONSENT_INH_MSG_MAX ||
        signer_sign(a->slot, g_pending_sign.bytes, g_pending_sign.len, sig) != 0) {
        send_denied_shown(a->id, "policy");
    } else {
        memset(g_inh.msg, 0, sizeof g_inh.msg);
        memcpy(g_inh.msg, g_pending_sign.bytes, g_pending_sign.len);
        g_inh.msg_len = g_pending_sign.len;
        memcpy(g_inh.sig, sig, 64);
        consent_commit_inheritance(&g_consent, a);
        persist_inh();
        send_line(proto_fmt_inheritance_set(g_out, sizeof g_out, a->id, sig));
        ui_notice("INHERITANCE", "armed", NULL, NOTICE_MS);
    }
    secure_zero(sig, sizeof sig);
    clear_pending_sign();
}

static void do_inheritance_clear(const consent_action_t *a)
{
    secure_zero(g_inh.msg, sizeof g_inh.msg);
    secure_zero(g_inh.sig, sizeof g_inh.sig);
    g_inh.msg_len = 0;
    persist_inh(); /* consent already disarmed ih.inh and raised ih_dirty */
    send_line(proto_fmt_ok(g_out, sizeof g_out, "inheritance_clear", a->id));
    ui_notice("INHERITANCE", "cleared", NULL, NOTICE_MS);
}

/* Fifth wrong rhythm: destroy everything except the inheritance region. consent_pin_result() already
 * reset the consent state (and left c->ih alone). */
static void do_wipe(const char *id)
{
    beacon_trigger();
    forget_real_pin();
    if (g_signer_ok) signer_wipe();
    signer_pin_close();
    clear_pending_sign();
    memset(&g_data, 0, sizeof g_data);
    g_consent.dirty = true;
    persist(); /* an all-zero main record: the device boots as new; g_inh is not zeroed */
    send_denied(id, "wiped");
    send_event("wiped", NULL);
    ui_notice("WIPED", "5 wrong PINs", "device reset", NOTICE_LONG_MS);
}

static void perform(const consent_action_t *a)
{
    switch (a->op) {
    case CONSENT_OP_SIGN: do_sign_now(a); break;
    case CONSENT_OP_INIT: do_init(a); break;
    case CONSENT_OP_INIT_WORDS: finish_init(a); break;
    case CONSENT_OP_ELEVATE:
        send_line(proto_fmt_elevated(g_out, sizeof g_out, a->id, CONSENT_ELEVATE_WINDOW_MS));
        ui_notice("ELEVATED", "one request", "within 2 minutes", NOTICE_MS);
        break;
    case CONSENT_OP_UNLOCK: send_line(proto_fmt_simple(g_out, sizeof g_out, "unlocked", a->id)); persist(); ui_idle(); break;
    case CONSENT_OP_POLICY:
        send_line(proto_fmt_ok(g_out, sizeof g_out, "policy", a->id));
        persist();
        if (a->policy.has_router_add || a->policy.has_router_remove) {
            /* the owner sees exactly which address is now trusted (or not) */
            tui_page_t pg;
            tui_build_router(a->policy.has_router_add ? "ROUTER ADDED" : "ROUTER REMOVED", &a->policy.router, &pg);
            tui_render(&g_display, &pg, TUI_FOOT_NONE, 1, 1, false);
            display_flush(&g_display);
            g_notice_until = now_ms() + NOTICE_LONG_MS;
        } else {
            ui_notice("POLICY", "limits set", NULL, NOTICE_MS);
        }
        break;
    case CONSENT_OP_ALLOWANCE: send_line(proto_fmt_ok(g_out, sizeof g_out, "allowance", a->id)); persist(); ui_notice("ALLOWANCE", "granted", NULL, NOTICE_MS); break;
    case CONSENT_OP_UPDATE_APPLY: do_update_apply(a); break;
    case CONSENT_OP_SET_PIN: do_set_pin(a); break;
    case CONSENT_OP_SET_DURESS_PIN: do_set_duress_pin(a); break;
    case CONSENT_OP_DECOY_ADDRESSES: do_decoy_addresses(a); break;
    case CONSENT_OP_INHERITANCE_SET: do_inheritance_set(a); break;
    case CONSENT_OP_INHERITANCE_CLEAR: do_inheritance_clear(a); break;
    default: break;
    }
}

/* A complete tap rhythm arrived while a request needed the PIN. */
static void handle_rhythm(const touch_rhythm_t *r, uint32_t now)
{
    uint8_t code[TOUCH_RHYTHM_CODE_LEN];
    consent_action_t act, res;
    touch_rhythm_encode(r, code);
    switch (consent_rhythm(&g_consent, code, now, &act)) {
    case CONSENT_RHYTHM_CONFIRM:
        send_line(proto_fmt_pending_confirm(g_out, sizeof g_out, g_consent.pending.id));
        ui_pending_prompt(0);
        break;
    case CONSENT_RHYTHM_MISMATCH:
        send_denied_shown(act.id, "pin_mismatch");
        break;
    case CONSENT_RHYTHM_SET:
        perform(&act);
        break;
    case CONSENT_RHYTHM_VERIFY: {
        /* unlock: both objects, every time, same pattern (signer_pin_verify2); everything else: the real one */
        consent_verdict_t verdict;
        int v = !g_signer_ok ? -1 : act.op == CONSENT_OP_UNLOCK ? signer_pin_verify2(act.pin) : signer_pin_verify(act.pin);
        if (v < 0) {
            /* The chip could not be asked: not a wrong PIN, do not count it. */
            consent_cancel_pending(&g_consent, &res);
            send_denied_shown(res.id, "policy");
            break;
        }
        verdict = v == SIGNER_VERIFY_REAL ? CONSENT_VERDICT_REAL : v == SIGNER_VERIFY_DURESS ? CONSENT_VERDICT_DURESS : CONSENT_VERDICT_WRONG;
        switch (consent_pin_verdict(&g_consent, verdict, now, &res)) {
        case CONSENT_PIN_OK:
            perform(&res); /* a duress unlock arrives here as a plain UNLOCK: same reply, same screen */
            break;
        case CONSENT_PIN_CONTINUE:
            /* set_duress_pin: the real rhythm is proven; the real session it opened is not needed */
            signer_pin_close();
            send_line(proto_fmt_pending(g_out, sizeof g_out, res.id, "pin", NULL));
            ui_pending_prompt(0);
            break;
        case CONSENT_PIN_WRONG:
            send_denied_shown(res.id, "wrong_pin");
            send_event("pin_failed", NULL);
            persist();
            break;
        case CONSENT_PIN_WIPE:
            do_wipe(res.id);
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    secure_zero(code, sizeof code);
    secure_zero(&act, sizeof act);
    secure_zero(&res, sizeof res);
}

/* ------------------------------------------------------------------ requests */
static void handle_hello(void)
{
    proto_hello_t h;
    memset(&h, 0, sizeof h);
    h.fw = PROTO_FW_VERSION;
    h.partition = g_boot_partition;
    h.update_staged = g_update.state == UPDATE_VERIFIED;
    h.trial = g_trial;
    h.se050 = signer_is_hardware() && g_signer_ok;
    h.initialized = g_consent.p.initialized;
    h.pubkey = h.initialized ? owner_pubkey() : NULL;
    h.locked = g_consent.locked;
    h.daily_limit = g_consent.p.daily_limit;
    h.spent_today = g_consent.p.spent_today;
    h.day = g_consent.p.day;
    h.allowance = (!g_consent.p.duress && g_consent.p.allowance.active) ? &g_consent.p.allowance : NULL;
    h.pin_set = g_consent.p.pin_set;
    h.duress_pin_set = g_consent.p.duress_pin_set;
    h.pin_failures = g_consent.p.pin_failures;
    h.dev_keys = signer_is_hardware() && g_signer_ok && !signer_keys_from_otp();
    h.inh_armed = g_consent.ih.inh.armed;
    h.inh_inactivity_days = g_consent.ih.inh.armed ? g_consent.ih.inh.inactivity_days : 0;
    h.inh_days_since_touch_tenths = consent_days_since_touch_tenths(&g_consent);
    h.evm_addr = h.initialized ? owner_evm_addr() : NULL;
    h.btc_pkh = h.initialized ? owner_btc_pkh() : NULL;
    h.daily_limit_wei = &g_consent.p.daily_limit_wei;
    h.spent_today_wei = &g_consent.p.spent_today_wei;
    h.daily_limit_sats = g_consent.p.daily_limit_sats;
    h.spent_today_sats = g_consent.p.spent_today_sats;
    h.display = display_present(&g_display);
    h.routers = &g_consent.p.routers;
    send_line(proto_fmt_hello(g_out, sizeof g_out, &h));
    if (g_boot_tamper) {
        g_boot_tamper = false;
        beacon_trigger();
        send_event("tamper", "brownout");
    }
}

/* A message of any kind must not be a transaction of any chain in disguise. */
static bool looks_like_any_tx(const uint8_t *bytes, size_t len)
{
    return solmsg_looks_like_message(bytes, len) || evmtx_looks_like_tx(bytes, len) || btcpsbt_looks_like_psbt(bytes, len);
}

static void stash_pending(const proto_req_t *r)
{
    clear_pending_sign();
    g_pending_sign.kind = r->sign.kind;
    memcpy(g_pending_sign.bytes, r->sign.bytes, r->sign.len);
    g_pending_sign.len = r->sign.len;
    g_pending_sign.valid = true;
}

static void handle_sign(const proto_req_t *r, uint32_t now)
{
    consent_action_t act;
    consent_reason_t why = CONSENT_DENY_NONE;
    consent_result_t res;
    solmsg_t msg;
    solmsg_summary_t parsed;
    evmtx_t evm;
    uint32_t eff_danger = 0;
    const uint8_t *owner = owner_pubkey();
    const char *need;
    bool have_parsed = false;
    uint8_t digest[32];
    tui_sign_t ui;

    if (!g_consent.p.initialized || !owner) { send_denied(r->id, "not_initialized"); return; }
    if (!g_signer_ok) { send_denied(r->id, "policy"); return; }
    memset(&ui, 0, sizeof ui);

    switch (r->sign.kind) {
    case PROTO_KIND_MSG:
        /* signMessage: never sign bytes that would also be a valid transaction of any chain. */
        if (looks_like_any_tx(r->sign.bytes, r->sign.len)) { send_denied(r->id, "policy"); return; }
        res = consent_begin_sign(&g_consent, r->id, 0, NULL, 0, false, r->sign.ttl_ms, now, &act, &why);
        break;
    case PROTO_KIND_TX: {
        policy_verdict_t v;
        if (solmsg_parse(r->sign.bytes, r->sign.len, &msg) != SOLMSG_OK) { send_denied(r->id, "policy"); return; }
        solmsg_analyze(&msg, owner, &parsed);
        have_parsed = true;
        v = policy_check(&parsed, &r->sign.claims, &g_consent.p.routers, &eff_danger); /* checks 1-4 */
        if (v != POLICY_OK) { send_denied_shown(r->id, policy_verdict_reason(v)); return; }
        res = consent_begin_sign(&g_consent, r->id, u64_sat_add(parsed.lamports_out, parsed.priority_lamports), (const uint8_t (*)[32])parsed.programs,
                                 parsed.n_programs, eff_danger != 0 || parsed.n_token_out > 0 || parsed.token_out_overflow,
                                 r->sign.ttl_ms, now, &act, &why); /* check 5; tokens never ride the allowance */
        break;
    }
    case PROTO_KIND_EVM_TX: {
        policy_verdict_t v;
        const uint8_t *addr = owner_evm_addr();
        if (!addr || !signer_secp256k1_present(consent_active_slot(&g_consent), SIGNER_CHAIN_EVM)) { send_denied(r->id, "not_initialized"); return; }
        if (evmtx_parse(r->sign.bytes, r->sign.len, &evm) != EVMTX_OK) { send_denied(r->id, "policy"); return; }
        evmtx_analyze(&evm, addr);
        v = policy_check_evm(&evm, addr, &r->sign.evm_claims, &g_consent.p.routers, &eff_danger); /* checks 1-5 */
        if (v != POLICY_OK) { send_denied_shown(r->id, policy_verdict_reason(v)); return; }
        res = consent_begin_sign_chain(&g_consent, r->id, CONSENT_CHAIN_EVM, 0, &evm.value, r->sign.ttl_ms, now, &why); /* check 6 */
        break;
    }
    case PROTO_KIND_BTC_PSBT: {
        policy_verdict_t v;
        uint8_t pub33[33];
        btcpsbt_t *p = &g_pending_sign.btc; /* large: parse straight into the static slot */
        if (!owner_btc_pkh() || !owner_btc_pub33(pub33)) { send_denied(r->id, "not_initialized"); return; }
        if (btcpsbt_parse(r->sign.bytes, r->sign.len, p) != BTCPSBT_OK ||
            btcpsbt_analyze(p, pub33, &g_data.btc_xpub[consent_active_slot(&g_consent)]) != BTCPSBT_OK) {
            clear_pending_sign();
            send_denied(r->id, "policy");
            return;
        }
        v = policy_check_btc(p, &r->sign.btc_claims, &eff_danger); /* checks 1-5 */
        if (v != POLICY_OK) { clear_pending_sign(); send_denied_shown(r->id, policy_verdict_reason(v)); return; }
        res = consent_begin_sign_chain(&g_consent, r->id, CONSENT_CHAIN_BTC, p->sats_out, NULL, r->sign.ttl_ms, now, &why); /* check 6 */
        break;
    }
    case PROTO_KIND_EVM_MSG:
        if (!owner_evm_addr()) { send_denied(r->id, "not_initialized"); return; }
        if (r->sign.typed.present) {
            if (r->sign.typed.rc == EIP712_E_UNSUPPORTED) { send_denied(r->id, "unsupported_typed_data"); return; }
            if (r->sign.typed.rc != EIP712_OK) { send_denied(r->id, "policy"); return; }
            if (memcmp(r->sign.typed.digest, r->sign.typed.claimed, 32) != 0) { send_denied_shown(r->id, "claims_mismatch"); return; }
            memcpy(digest, r->sign.typed.digest, 32);
        } else {
            if (looks_like_any_tx(r->sign.bytes, r->sign.len)) { send_denied(r->id, "policy"); return; }
            evmtx_personal_digest(r->sign.bytes, r->sign.len, digest);
        }
        res = consent_begin_sign_chain(&g_consent, r->id, CONSENT_CHAIN_EVM, 0, NULL, r->sign.ttl_ms, now, &why);
        break;
    case PROTO_KIND_BTC_MSG: {
        const uint8_t *pkh = owner_btc_pkh();
        if (!pkh) { send_denied(r->id, "not_initialized"); return; }
        if (looks_like_any_tx(r->sign.bytes, r->sign.len)) { send_denied(r->id, "policy"); return; }
        btcpsbt_bip322_digest(r->sign.bytes, r->sign.len, pkh, digest);
        res = consent_begin_sign_chain(&g_consent, r->id, CONSENT_CHAIN_BTC, 0, NULL, r->sign.ttl_ms, now, &why);
        break;
    }
    default:
        send_denied(r->id, "policy");
        return;
    }
    if (res == CONSENT_R_DENIED) { clear_pending_sign(); send_denied_shown(r->id, consent_reason_str(why)); return; }

    if (r->sign.kind == PROTO_KIND_BTC_PSBT) {
        /* the parsed PSBT is already in place; keep it, stash the bytes around it */
        btcpsbt_t keep = g_pending_sign.btc;
        stash_pending(r);
        g_pending_sign.btc = keep;
    } else {
        stash_pending(r);
    }
    if (have_parsed) { g_pending_sign.parsed = parsed; g_pending_sign.has_parsed = true; }
    if (r->sign.kind == PROTO_KIND_EVM_MSG || r->sign.kind == PROTO_KIND_BTC_MSG) memcpy(g_pending_sign.digest, digest, 32);

    if (res == CONSENT_R_DONE) { /* inside the allowance: no touch (Solana only) */
        do_sign_now(&act);
        return;
    }

    /* The trusted screen, from OUR parse (never from claims / summary text), BEFORE the request can be approved. */
    switch (r->sign.kind) {
    case PROTO_KIND_TX: ui.kind = TUI_SIGN_SOL_TX; ui.sol = &parsed; ui.sol_dangerous = eff_danger; break;
    case PROTO_KIND_MSG: ui.kind = TUI_SIGN_SOL_MSG; ui.msg = g_pending_sign.bytes; ui.msg_len = g_pending_sign.len; break;
    case PROTO_KIND_EVM_TX: ui.kind = TUI_SIGN_EVM_TX; ui.evm = &evm; ui.evm_dangerous = eff_danger; break;
    case PROTO_KIND_BTC_PSBT: ui.kind = TUI_SIGN_BTC_PSBT; ui.btc = &g_pending_sign.btc; ui.btc_dangerous = eff_danger; break;
    case PROTO_KIND_EVM_MSG:
        if (r->sign.typed.present) { ui.kind = TUI_SIGN_EVM_TYPED; ui.digest = g_pending_sign.digest; }
        else { ui.kind = TUI_SIGN_EVM_MSG; ui.msg = g_pending_sign.bytes; ui.msg_len = g_pending_sign.len; }
        break;
    case PROTO_KIND_BTC_MSG: ui.kind = TUI_SIGN_BTC_MSG; ui.msg = g_pending_sign.bytes; ui.msg_len = g_pending_sign.len; break;
    default: break;
    }
    tui_build_sign(&ui, &g_doc);
    if (g_doc.truncated || g_doc.n_pages == 0) {
        /* could not show everything the owner would be consenting to: refuse rather than sign blind */
        consent_cancel_pending(&g_consent, NULL);
        clear_pending_sign();
        send_denied_shown(r->id, "policy");
        return;
    }
    g_doc_foot = TUI_FOOT_SIGN;
    g_doc_cancelable = true;
    ui_sign_page(0); /* -> pending(touch) */
    need = consent_need_str(consent_pending_need(&g_consent));
    switch (r->sign.kind) {
    case PROTO_KIND_EVM_TX: send_line(proto_fmt_pending_evm(g_out, sizeof g_out, r->id, need, &evm, eff_danger)); break;
    case PROTO_KIND_BTC_PSBT: send_line(proto_fmt_pending_btc(g_out, sizeof g_out, r->id, need, &g_pending_sign.btc, eff_danger)); break;
    case PROTO_KIND_TX: send_line(proto_fmt_pending_sol(g_out, sizeof g_out, r->id, need, &parsed, eff_danger)); break;
    default: send_line(proto_fmt_pending(g_out, sizeof g_out, r->id, need, NULL)); break;
    }
}

/* inheritance_set: the same parse + claims checks as a tx sign, plus: durable nonce first, nonce account
 * != owner, message small enough for the chip and for the inheritance record. */
static void handle_inheritance_set(const proto_req_t *r, uint32_t now)
{
    consent_reason_t why = CONSENT_DENY_NONE;
    consent_result_t res;
    solmsg_t msg;
    solmsg_summary_t parsed;
    policy_verdict_t v;
    uint8_t nonce_account[32];
    bool durable, replacing;
    uint32_t eff_danger = 0;
    tui_sign_t ui;
    const uint8_t *owner = owner_pubkey();

    if (!g_consent.p.initialized || !owner) { send_denied(r->id, "not_initialized"); return; }
    if (!g_signer_ok) { send_denied(r->id, "policy"); return; }
    if (r->sign.len > CONSENT_INH_MSG_MAX) { send_denied(r->id, "policy"); return; }
    if (solmsg_parse(r->sign.bytes, r->sign.len, &msg) != SOLMSG_OK) { send_denied(r->id, "policy"); return; }
    if (msg.num_required_signatures > CONSENT_INH_MAX_SIGNERS) { send_denied(r->id, "policy"); return; }
    solmsg_analyze(&msg, owner, &parsed);
    v = policy_check(&parsed, &r->sign.claims, &g_consent.p.routers, &eff_danger); /* checks 1-4 */
    if (v != POLICY_OK) { send_denied_shown(r->id, policy_verdict_reason(v)); return; }
    replacing = g_consent.ih.inh.armed;
    durable = solmsg_starts_with_advance_nonce(&msg, nonce_account) && memcmp(nonce_account, owner, 32) != 0;
    {
        consent_inheritance_t req = r->inheritance;
        memcpy(req.signer, g_data.pubkey[CONSENT_SLOT_REAL], 32); /* owner == real key here: gate() refuses duress */
        res = consent_begin_inheritance_set(&g_consent, r->id, &req, durable, now, &why);
    }
    if (res != CONSENT_R_PENDING) { send_denied_shown(r->id, consent_reason_str(why)); return; }
    clear_pending_sign();
    memcpy(g_pending_sign.bytes, r->sign.bytes, r->sign.len);
    g_pending_sign.len = r->sign.len;
    g_pending_sign.valid = true;
    g_pending_sign.parsed = parsed;
    g_pending_sign.has_parsed = true;
    memset(&ui, 0, sizeof ui);
    ui.kind = TUI_SIGN_INHERITANCE; ui.sol = &parsed; ui.sol_dangerous = eff_danger;
    ui.inh_days = r->inheritance.inactivity_days; ui.inh_heir = r->inheritance.heir; ui.inh_replace = replacing;
    tui_build_sign(&ui, &g_doc);
    if (g_doc.truncated || g_doc.n_pages == 0) {
        consent_cancel_pending(&g_consent, NULL);
        clear_pending_sign();
        send_denied_shown(r->id, "policy");
        return;
    }
    g_doc_foot = tui_sign_footer(&ui);
    g_doc_cancelable = replacing; /* arming waits for the long touch; a replace can be cancelled by holding */
    ui_sign_page(0);
    send_line(proto_fmt_pending_sol(g_out, sizeof g_out, r->id, consent_need_str(consent_pending_need(&g_consent)), &parsed, eff_danger));
}

static void handle_inheritance_status(const char *id)
{
    proto_inh_status_t st;
    uint64_t pd = g_consent.ih.powered_seconds / CONSENT_SECONDS_PER_DAY, pt = (g_consent.ih.powered_seconds % CONSENT_SECONDS_PER_DAY) * 10u / CONSENT_SECONDS_PER_DAY;
    uint64_t v = pd * 10u + pt;
    st.inh = &g_consent.ih.inh;
    st.days_since_touch_tenths = consent_days_since_touch_tenths(&g_consent);
    st.powered_days_tenths = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
    send_line(proto_fmt_inheritance_status(g_out, sizeof g_out, id, &st));
}

/* Wire transaction = shortvec(num_required_signatures) || sig[0] || zero sigs for other signers || message.
 * The owner is the fee payer (policy check 1), so our signature is the first one. */
static void handle_inheritance_release(const char *id)
{
    static uint8_t tx[1 + 64 * CONSENT_INH_MAX_SIGNERS + CONSENT_INH_MSG_MAX];
    consent_reason_t why = CONSENT_DENY_NONE;
    size_t o = 0;
    uint8_t nsig, i;
    if (consent_inheritance_release(&g_consent, &why) != CONSENT_R_DONE) { send_denied(id, consent_reason_str(why)); return; }
    if (g_inh.msg_len == 0 || g_inh.msg_len > CONSENT_INH_MSG_MAX) { send_denied(id, "not_armed"); return; }
    nsig = (g_inh.msg[0] & 0x80) ? g_inh.msg[1] : g_inh.msg[0];
    if (nsig == 0 || nsig > CONSENT_INH_MAX_SIGNERS) { send_denied(id, "policy"); return; }
    tx[o++] = nsig;
    memcpy(tx + o, g_inh.sig, 64); o += 64;
    for (i = 1; i < nsig; i++) { memset(tx + o, 0, 64); o += 64; }
    memcpy(tx + o, g_inh.msg, g_inh.msg_len); o += g_inh.msg_len;
    send_line(proto_fmt_inheritance_release(g_out, sizeof g_out, id, tx, o));
    secure_zero(tx, sizeof tx);
}

static void reply_pending_or_denied(consent_result_t res, const char *id, consent_reason_t why)
{
    if (res == CONSENT_R_PENDING) {
        ui_pending_prompt(0);
        send_line(proto_fmt_pending(g_out, sizeof g_out, id, consent_need_str(consent_pending_need(&g_consent)), NULL));
    } else {
        send_denied_shown(id, consent_reason_str(why));
    }
}

/* A pending request was cancelled from the outside (lock, duress): a half-done init must destroy its keys. */
static void on_cancelled(const consent_action_t *cancelled, const char *reason)
{
    if (cancelled->op == CONSENT_OP_INIT_WORDS) abort_init(reason);
    else send_denied(cancelled->id, reason);
}

static void handle_line(const char *line)
{
    uint32_t now = now_ms();
    consent_action_t cancelled;
    consent_reason_t why = CONSENT_DENY_NONE;
    consent_result_t res;
    int rc = proto_parse_request(line, &g_req);
    if (rc != PROTO_OK) {
        send_line(proto_fmt_error(g_out, sizeof g_out, g_req.id, proto_error_str(rc)));
        return;
    }
    switch (g_req.type) {
    case PROTO_REQ_HELLO:
        handle_hello();
        break;
    case PROTO_REQ_TIME:
        switch (consent_set_time(&g_consent, g_req.unix_time, now)) {
        case CONSENT_TIME_CHANGED: persist(); break;
        case CONSENT_TIME_REJECTED: send_event("time_rejected", NULL); break; /* backwards or > 36 h ahead: ignored */
        default: break;
        }
        break;
    case PROTO_REQ_REVEAL_SEED:
        /* Deliberate non-feature: the words are shown on the trusted display at init and never cross USB. */
        send_denied(g_req.id, "not_supported");
        break;
    case PROTO_REQ_SET_DURESS_PIN: {
        /* right after set_pin the real rhythm is still known here: init flow step 3 skips re-entering it */
        bool fresh = g_real_pin_valid && (int32_t)(now - g_real_pin_until) < 0;
        res = consent_begin_set_duress_pin(&g_consent, g_req.id, fresh ? g_real_pin : NULL, now, &why);
        forget_real_pin();
        reply_pending_or_denied(res, g_req.id, why);
        break;
    }
    case PROTO_REQ_DECOY_ADDRESSES:
        forget_real_pin();
        res = consent_begin_decoy_addresses(&g_consent, g_req.id, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_INIT:
        forget_real_pin();
        res = consent_begin_init(&g_consent, g_req.id, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_SET_PIN:
        res = consent_begin_set_pin(&g_consent, g_req.id, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_SIGN:
        forget_real_pin();
        handle_sign(&g_req, now);
        break;
    case PROTO_REQ_ELEVATE:
        forget_real_pin();
        res = consent_begin_elevate_chain(&g_consent, g_req.id, g_req.elevate_chain, g_req.limit_lamports, &g_req.limit_wei, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_LOCK:
        forget_real_pin();
        if (consent_lock(&g_consent, &cancelled)) on_cancelled(&cancelled, "locked");
        clear_pending_sign();
        signer_pin_close(); /* the chip session ends with the lock */
        send_line(proto_fmt_simple(g_out, sizeof g_out, "locked", NULL));
        persist();
        ui_idle();
        break;
    case PROTO_REQ_UNLOCK:
        forget_real_pin();
        res = consent_begin_unlock(&g_consent, g_req.id, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_POLICY:
        forget_real_pin();
        res = consent_begin_policy_multi(&g_consent, g_req.id, &g_req.policy, now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    case PROTO_REQ_DURESS:
        forget_real_pin();
        if (consent_duress(&g_consent, &cancelled)) on_cancelled(&cancelled, "user");
        clear_pending_sign();
        beacon_trigger();
        send_line(proto_fmt_ok(g_out, sizeof g_out, "duress", g_req.id));
        persist();
        ui_idle(); /* shows the decoy addresses as if nothing happened */
        break;
    case PROTO_REQ_ALLOWANCE:
        forget_real_pin();
        if (g_req.allowance.revoke) {
            consent_allowance_revoke(&g_consent);
            send_line(proto_fmt_ok(g_out, sizeof g_out, "allowance", g_req.id));
            persist();
            ui_notice("ALLOWANCE", "revoked", NULL, NOTICE_MS);
        } else {
            res = consent_begin_allowance(&g_consent, g_req.id, &g_req.allowance.a, now, &why);
            reply_pending_or_denied(res, g_req.id, why);
        }
        break;
    case PROTO_REQ_INHERITANCE_SET:
        forget_real_pin();
        handle_inheritance_set(&g_req, now);
        break;
    case PROTO_REQ_UPDATE_BEGIN:
    case PROTO_REQ_UPDATE_CHUNK:
    case PROTO_REQ_UPDATE_END:
    case PROTO_REQ_UPDATE_APPLY:
    case PROTO_REQ_UPDATE_COMMIT:
    case PROTO_REQ_UPDATE_ABORT:
        handle_update();
        break;
    case PROTO_REQ_INHERITANCE_STATUS:
        handle_inheritance_status(g_req.id);
        break;
    case PROTO_REQ_INHERITANCE_RELEASE:
        handle_inheritance_release(g_req.id);
        break;
    case PROTO_REQ_INHERITANCE_CLEAR:
        forget_real_pin();
        res = consent_begin_inheritance_clear(&g_consent, g_req.id, g_data.pubkey[CONSENT_SLOT_REAL], now, &why);
        reply_pending_or_denied(res, g_req.id, why);
        break;
    default:
        send_line(proto_fmt_error(g_out, sizeof g_out, g_req.id, "unknown_type"));
        break;
    }
    secure_zero(&g_req, sizeof g_req);
}

/* ------------------------------------------------------------------ periodic */
static bool tick_cb(struct repeating_timer *t)
{
    (void)t;
    g_ticks++; /* ISR does nothing else */
    return true;
}

static void update_led(uint32_t now)
{
    bool on;
    consent_need_t need = consent_pending_need(&g_consent);
    if (touch_is_down(&g_touch)) on = true;
    else if (need == CONSENT_NEED_LONG_TOUCH) on = (now / 100) & 1;  /* fast blink: hold for 3 s */
    else if (need == CONSENT_NEED_PIN) on = (now % 1000) < 100 || ((now % 1000) >= 200 && (now % 1000) < 300); /* double blink: tap the rhythm */
    else if (need == CONSENT_NEED_TOUCH || need == CONSENT_NEED_DISPLAY) on = (now / 250) & 1; /* slow blink: touch */
    else if (g_consent.locked) on = false;
    else on = (now % 3000) < 30;                                      /* idle heartbeat */
    gpio_put(PIN_LED, on);
}

/* A short or long touch: approve, turn a page, or cancel, as the consent machine decides. */
static void handle_touch(bool long_touch, uint32_t now)
{
    consent_action_t act;
    switch (consent_touch_ex(&g_consent, long_touch, now, &act)) {
    case CONSENT_TOUCH_APPROVED:
        perform(&act);
        break;
    case CONSENT_TOUCH_PAGE:
        if (act.op == CONSENT_OP_INIT_WORDS) ui_words_page(act.page);
        else ui_sign_page(act.page);
        break;
    case CONSENT_TOUCH_CANCELLED:
        if (act.op == CONSENT_OP_INIT_WORDS) {
            abort_init("user");
        } else {
            clear_pending_sign();
            send_denied_shown(act.id, "user");
        }
        break;
    default:
        break;
    }
    secure_zero(&act, sizeof act);
}

static void on_tick(void)
{
    uint32_t now = now_ms();
    consent_action_t act;
    beacon_service(now);
    touch_event_t ev = touch_update(&g_touch, gpio_get(PIN_TOUCH), now);
    switch (ev) {
    case TOUCH_EV_PRESS:
        consent_touch_seen(&g_consent); /* proof of life for the inheritance timer, rhythm taps included */
        send_event("touch", NULL);
        break;
    case TOUCH_EV_SHORT:
        handle_touch(false, now);
        break;
    case TOUCH_EV_LONG:
        send_event("long_touch", NULL);
        handle_touch(true, now);
        break;
    default:
        break;
    }
    /* PIN rhythm: while a request needs the PIN, every press is a tap. */
    if (consent_pending_need(&g_consent) == CONSENT_NEED_PIN) {
        touch_rhythm_t r;
        if (!g_rhythm.active) touch_rhythm_start(&g_rhythm);
        switch (touch_rhythm_update(&g_rhythm, ev == TOUCH_EV_PRESS, now, &r)) {
        case TOUCH_RHYTHM_DONE: handle_rhythm(&r, now); break;
        case TOUCH_RHYTHM_TOO_FEW: send_event("rhythm", "too_few"); ui_pending_prompt(0); break;
        case TOUCH_RHYTHM_TAP: ui_pending_prompt(g_rhythm.n_taps); break; /* one more dot, never the gaps */
        default: break;
        }
        secure_zero(&r, sizeof r);
    } else if (g_rhythm.active) {
        touch_rhythm_stop(&g_rhythm);
    }
    if (consent_tick(&g_consent, now, &act)) {
        if (act.op == CONSENT_OP_INIT_WORDS) {
            abort_init("timeout");
        } else {
            send_denied_shown(act.id, "timeout");
            if (act.op == CONSENT_OP_SIGN || act.op == CONSENT_OP_INHERITANCE_SET) clear_pending_sign();
        }
    }
    if (g_notice_until && (int32_t)(now - g_notice_until) >= 0) ui_refresh(); /* notice over: back to the state screen */
    {
        uint32_t days_left;
        while (consent_inheritance_warning(&g_consent, &days_left))
            send_line(proto_fmt_event_days_left(g_out, sizeof g_out, days_left));
    }
    if (g_consent.dirty || g_consent.ih_dirty) persist(); /* allowance expired, hourly checkpoint, warning */
    update_led(now);
}

static void check_usb(void)
{
    bool up = stdio_usb_connected();
    if (up && !g_usb_was_up && g_usb_ever_up) {
        /* The host side of the link went away and came back while we were running. */
        beacon_trigger();
        send_event("tamper", "usb_reset");
    }
    if (up) g_usb_ever_up = true;
    g_usb_was_up = up;
}

static void detect_boot_reason(void)
{
    if (watchdog_caused_reboot()) return; /* our own doing */
#if PICO_RP2040
    {
        uint32_t r = vreg_and_chip_reset_hw->chip_reset;
        /* Not a clean power-on: RUN pin pulled or power-manager restart -> report as tamper. A real
         * brown-out shows as HAD_POR and cannot be told apart from a normal plug-in. */
        if ((r & (VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS | VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS)) &&
            !(r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS))
            g_boot_tamper = true;
    }
#elif PICO_RP2350
    {
        uint32_t r = powman_hw->chip_reset;
        /* RUN pin pulled, debugger reset request, rescue reset or the glitch detector tripping, without a
         * power-on reset -> somebody poked the board. */
        if ((r & (POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS | POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS |
                  POWMAN_CHIP_RESET_HAD_RESCUE_BITS | POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)) &&
            !(r & POWMAN_CHIP_RESET_HAD_POR_BITS))
            g_boot_tamper = true;
    }
#endif
}

int main(void)
{
    struct repeating_timer timer;
    bool loaded;
    uint8_t slot;

    stdio_init_all();
    stdio_set_translate_crlf(&stdio_usb, false);

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_TOUCH);
    gpio_set_dir(PIN_TOUCH, GPIO_IN);
    gpio_pull_down(PIN_TOUCH); /* TTP223 drives high on touch */
    gpio_init(PIN_BEACON);
    gpio_set_dir(PIN_BEACON, GPIO_OUT);
    gpio_put(PIN_BEACON, 0);

    detect_boot_reason();
    update_boot_check();
    rng_init();
    display_hw_init();
    touch_init(&g_touch);
    touch_rhythm_stop(&g_rhythm);
    proto_framer_init(&g_framer);

    storage_init(&g_storage, &g_flash_ops);
    loaded = storage_load(&g_storage, &g_data);
    consent_init(&g_consent, loaded ? &g_data.consent : NULL, now_ms());
    storage_inh_init(&g_inh_storage, &g_inh_flash_ops);
    if (!storage_inh_load(&g_inh_storage, &g_inh)) memset(&g_inh, 0, sizeof g_inh);
    consent_init_inheritance(&g_consent, &g_inh.ih); /* counter resumes from flash, never rewinds */

    g_signer_ok = signer_init() == 0;
    if (g_signer_ok) {
        signer_soft_load((const uint8_t (*)[32])g_data.signer_blob, g_data.pin_hash); /* software signer only */
        signer_soft_load_secp((const uint8_t (*)[SIGNER_SECP_CHAINS][32])g_data.secp_blob);
        signer_soft_load_duress(g_data.duress_hash, g_data.duress_salt);
        for (slot = 0; slot < SIGNER_SLOTS; slot++) {
            uint8_t pk[32];
            /* The signing back-end is the source of truth for the public keys. */
            if (signer_slot_present(slot) && signer_pubkey(slot, pk) == 0 && memcmp(pk, g_data.pubkey[slot], 32) != 0) {
                memcpy(g_data.pubkey[slot], pk, 32);
                g_consent.dirty = true;
            }
            refresh_addresses(slot);
        }
        if (g_consent.p.initialized && !signer_slot_present(CONSENT_SLOT_REAL)) {
            /* Flash says initialised but the key is gone (chip swapped / erased): refuse to sign. */
            g_signer_ok = false;
        }
        if (g_consent.p.pin_set && !signer_pin_present()) {
            /* Flash says a PIN exists but the chip has no PIN object: same story. */
            g_signer_ok = false;
        }
        if (g_consent.p.duress_pin_set != signer_duress_pin_present()) {
            /* the back-end is the source of truth for the second object too */
            g_consent.p.duress_pin_set = signer_duress_pin_present();
            g_consent.dirty = true;
        }
        persist();
    }
    ui_idle();

    add_repeating_timer_ms(-TICK_MS, tick_cb, NULL, &timer);

    for (;;) {
        static uint32_t last_tick;
        uint32_t t;
        int i;
        for (i = 0; i < 512; i++) {
            const char *line;
            int ch = getchar_timeout_us(0);
            if (ch < 0) break;
            if (proto_framer_push(&g_framer, (char)ch, &line)) handle_line(line);
        }
        t = g_ticks;
        if (t != last_tick) {
            last_tick = t;
            on_tick();
            check_usb();
        } else {
            sleep_us(500);
        }
    }
}
