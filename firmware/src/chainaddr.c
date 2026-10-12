#include "chainaddr.h"

#include <string.h>

#include "keccak.h"
#include "ripemd160.h"
#include "secp256k1.h"
#include "segwit_addr.h"
#include "sha256.h"
#include "util.h"

static const char HEX[] = "0123456789abcdef";

void evm_address_from_pubkey(const uint8_t pub[64], uint8_t addr[20])
{
    uint8_t h[32];
    keccak256(pub, 64, h);
    memcpy(addr, h + 12, 20);
}

void evm_address_to_hex(const uint8_t addr[20], char out[EVM_ADDR_HEX_LEN])
{
    char lower[40];
    uint8_t h[32];
    int i;
    for (i = 0; i < 20; i++) {
        lower[2 * i] = HEX[addr[i] >> 4];
        lower[2 * i + 1] = HEX[addr[i] & 15];
    }
    keccak256((const uint8_t *)lower, 40, h);
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 40; i++) {
        char c = lower[i];
        int nib = (h[i / 2] >> (i % 2 ? 0 : 4)) & 15;
        if (c >= 'a' && c <= 'f' && nib >= 8) c = (char)(c - 'a' + 'A');
        out[2 + i] = c;
    }
    out[42] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool evm_address_from_hex(const char *s, uint8_t addr[20])
{
    int i;
    bool has_upper = false, has_lower = false;
    if (!s || strlen(s) != 42 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    for (i = 0; i < 40; i++) {
        int hi = hexval(s[2 + i]);
        if (hi < 0) return false;
        if (s[2 + i] >= 'A' && s[2 + i] <= 'F') has_upper = true;
        if (s[2 + i] >= 'a' && s[2 + i] <= 'f') has_lower = true;
        if (i % 2 == 0) addr[i / 2] = (uint8_t)(hi << 4);
        else addr[i / 2] |= (uint8_t)hi;
    }
    if (has_upper && has_lower) {
        char chk[EVM_ADDR_HEX_LEN];
        evm_address_to_hex(addr, chk);
        if (memcmp(chk + 2, s + 2, 40) != 0) return false; /* mixed case that is not the EIP-55 checksum */
    }
    return true;
}

void btc_pkh_from_pubkey(const uint8_t pub[64], uint8_t pkh[20])
{
    uint8_t c[33];
    secp256k1_compress(pub, c);
    hash160(c, 33, pkh);
}

void btc_p2wpkh_script(const uint8_t pkh[20], uint8_t script[22])
{
    script[0] = 0x00;
    script[1] = 0x14;
    memcpy(script + 2, pkh, 20);
}

bool btc_p2wpkh_address(const uint8_t pkh[20], char out[BTC_ADDR_MAX])
{
    return segwit_addr_encode(out, "bc", 0, pkh, 20) == 1;
}

static bool base58check(uint8_t version, const uint8_t payload[20], char out[BTC_ADDR_MAX])
{
    uint8_t buf[25], h[32];
    buf[0] = version;
    memcpy(buf + 1, payload, 20);
    sha256d(buf, 21, h);
    memcpy(buf + 21, h, 4);
    return base58_encode(buf, 25, out, BTC_ADDR_MAX) > 0;
}

bool btc_script_to_address(const uint8_t *script, size_t len, char out[BTC_ADDR_MAX])
{
    out[0] = 0;
    if (len >= 4 && len <= 42 && (script[0] == 0x00 || (script[0] >= 0x51 && script[0] <= 0x60)) &&
        script[1] == len - 2 && len - 2 >= 2 && len - 2 <= 40) {
        int ver = script[0] == 0 ? 0 : script[0] - 0x50;
        if (ver == 0 && len - 2 != 20 && len - 2 != 32) return false;
        return segwit_addr_encode(out, "bc", ver, script + 2, len - 2) == 1;
    }
    if (len == 25 && script[0] == 0x76 && script[1] == 0xa9 && script[2] == 0x14 && script[23] == 0x88 && script[24] == 0xac)
        return base58check(0x00, script + 3, out);
    if (len == 23 && script[0] == 0xa9 && script[1] == 0x14 && script[22] == 0x87) return base58check(0x05, script + 2, out);
    return false;
}

size_t btc_address_to_script(const char *addr, uint8_t script[42])
{
    int ver;
    uint8_t prog[40];
    size_t plen;
    if (!addr || !addr[0]) return 0;
    if ((addr[0] == 'b' || addr[0] == 'B') && (addr[1] == 'c' || addr[1] == 'C') && addr[2] == '1') {
        if (segwit_addr_decode(&ver, prog, &plen, "bc", addr) != 1) return 0;
        if (ver == 0 && plen != 20 && plen != 32) return 0;
        script[0] = ver == 0 ? 0x00 : (uint8_t)(0x50 + ver);
        script[1] = (uint8_t)plen;
        memcpy(script + 2, prog, plen);
        return 2 + plen;
    }
    {
        uint8_t buf[32], h[32];
        int n = base58_decode(addr, strlen(addr), buf, sizeof buf);
        if (n != 25) return 0;
        sha256d(buf, 21, h);
        if (memcmp(h, buf + 21, 4) != 0) return 0;
        if (buf[0] == 0x00) {
            script[0] = 0x76; script[1] = 0xa9; script[2] = 0x14;
            memcpy(script + 3, buf + 1, 20);
            script[23] = 0x88; script[24] = 0xac;
            return 25;
        }
        if (buf[0] == 0x05) {
            script[0] = 0xa9; script[1] = 0x14;
            memcpy(script + 2, buf + 1, 20);
            script[22] = 0x87;
            return 23;
        }
    }
    return 0;
}
