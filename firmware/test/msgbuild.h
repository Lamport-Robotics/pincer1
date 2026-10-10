/* Hand-built Solana message fixtures for tests (legacy + v0 wire format). */
#ifndef PINCER_TEST_MSGBUILD_H
#define PINCER_TEST_MSGBUILD_H
#include <stdint.h>
#include <string.h>

#include "solmsg.h"
#include "util.h"

typedef struct {
    uint8_t buf[SOLMSG_MAX_BYTES + 64];
    size_t len;
} mb_t;

static void mb_u8(mb_t *m, uint8_t v) { m->buf[m->len++] = v; }
static void mb_bytes(mb_t *m, const void *p, size_t n) { memcpy(m->buf + m->len, p, n); m->len += n; }
static void mb_cu16(mb_t *m, uint16_t v)
{
    while (v >= 0x80) { mb_u8(m, (uint8_t)(v | 0x80)); v >>= 7; }
    mb_u8(m, (uint8_t)v);
}
/* version: -1 legacy, 0 = v0 */
static void mb_begin(mb_t *m, int version, uint8_t nsig, uint8_t ro_signed, uint8_t ro_unsigned,
                     const uint8_t (*keys)[32], uint8_t nkeys)
{
    uint8_t bh[32];
    size_t i;
    for (i = 0; i < 32; i++) bh[i] = (uint8_t)(0xB0 + i);
    m->len = 0;
    if (version == 0) mb_u8(m, 0x80);
    mb_u8(m, nsig); mb_u8(m, ro_signed); mb_u8(m, ro_unsigned);
    mb_cu16(m, nkeys);
    mb_bytes(m, keys, (size_t)nkeys * 32);
    mb_bytes(m, bh, 32);
}
static void mb_n_ix(mb_t *m, uint8_t n) { mb_cu16(m, n); }
static void mb_ix(mb_t *m, uint8_t prog, const uint8_t *accts, uint8_t na, const uint8_t *data, size_t dl)
{
    mb_u8(m, prog);
    mb_cu16(m, na);
    mb_bytes(m, accts, na);
    mb_cu16(m, (uint16_t)dl);
    mb_bytes(m, data, dl);
}
static void mb_n_lookups(mb_t *m, uint8_t n) { mb_cu16(m, n); }
static void mb_lookup(mb_t *m, const uint8_t key[32], const uint8_t *wr, uint8_t nw, const uint8_t *ro, uint8_t nr)
{
    mb_bytes(m, key, 32);
    mb_cu16(m, nw); mb_bytes(m, wr, nw);
    mb_cu16(m, nr); mb_bytes(m, ro, nr);
}

/* instruction data */
static size_t ix_sys_transfer(uint8_t *d, uint64_t lamports) { wr_le32(d, 2); wr_le64(d + 4, lamports); return 12; }
static size_t ix_sys_create_account(uint8_t *d, uint64_t lamports, uint64_t space, const uint8_t owner[32])
{
    wr_le32(d, 0); wr_le64(d + 4, lamports); wr_le64(d + 12, space); memcpy(d + 20, owner, 32); return 52;
}
static size_t ix_sys_assign(uint8_t *d, const uint8_t owner[32]) { wr_le32(d, 1); memcpy(d + 4, owner, 32); return 36; }
static size_t ix_sys_transfer_with_seed(uint8_t *d, uint64_t lamports)
{
    static const char seed[] = "seed";
    size_t o = 0;
    wr_le32(d, 11); o = 4; wr_le64(d + o, lamports); o += 8;
    wr_le64(d + o, 4); o += 8; memcpy(d + o, seed, 4); o += 4;
    memset(d + o, 0, 32); o += 32; /* from_owner */
    return o;
}
static size_t ix_sys_create_with_seed(uint8_t *d, const uint8_t base[32], uint64_t lamports)
{
    size_t o = 0;
    wr_le32(d, 3); o = 4; memcpy(d + o, base, 32); o += 32;
    wr_le64(d + o, 3); o += 8; memcpy(d + o, "abc", 3); o += 3;
    wr_le64(d + o, lamports); o += 8; wr_le64(d + o, 0); o += 8; memset(d + o, 0, 32); o += 32;
    return o;
}
static size_t ix_tok_transfer(uint8_t *d, uint64_t amount) { d[0] = 3; wr_le64(d + 1, amount); return 9; }
static size_t ix_tok_transfer_checked(uint8_t *d, uint64_t amount, uint8_t dec) { d[0] = 12; wr_le64(d + 1, amount); d[9] = dec; return 10; }
static size_t ix_tok_approve(uint8_t *d, uint64_t amount) { d[0] = 4; wr_le64(d + 1, amount); return 9; }
static size_t ix_tok_set_authority(uint8_t *d, const uint8_t newauth[32]) { d[0] = 6; d[1] = 2; d[2] = 1; memcpy(d + 3, newauth, 32); return 35; }
static size_t ix_tok_close(uint8_t *d) { d[0] = 9; return 1; }
static size_t ix_tok_burn(uint8_t *d, uint64_t amount) { d[0] = 8; wr_le64(d + 1, amount); return 9; }

static void fill_key(uint8_t k[32], uint8_t tag) { memset(k, tag, 32); }
#endif
