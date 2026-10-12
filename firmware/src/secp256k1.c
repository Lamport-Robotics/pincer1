#include "secp256k1.h"

#include <string.h>

#include "sha256.h"
#include "uECC.h"
#include "uECC_vli.h"
#include "util.h"

#define SECP_WORDS (32 / uECC_WORD_SIZE) /* 8 on Cortex-M (32-bit words), 4 on a 64-bit host */
typedef uECC_word_t vli_t[SECP_WORDS];

static uECC_Curve C(void) { return uECC_secp256k1(); }
static wordcount_t nw(void) { return (wordcount_t)uECC_curve_num_words(C()); }

static void to_vli(uECC_word_t *v, const uint8_t be[32]) { uECC_vli_bytesToNative(v, be, 32); }
static void from_vli(uint8_t be[32], const uECC_word_t *v) { uECC_vli_nativeToBytes(be, 32, v); }

static bool scalar_ok(const uECC_word_t *k)
{
    return !uECC_vli_isZero(k, nw()) && uECC_vli_cmp(uECC_curve_n(C()), k, nw()) == 1;
}

bool secp256k1_scalar_valid(const uint8_t k[32])
{
    vli_t v;
    bool ok;
    to_vli(v, k);
    ok = scalar_ok(v);
    secure_zero(v, sizeof v);
    return ok;
}

bool secp256k1_scalar_add(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    vli_t va, vb, vr;
    bool ok;
    to_vli(va, a);
    to_vli(vb, b);
    /* a may be 0 here? No: a is the parent key (valid scalar) and b = IL (must be < n). */
    ok = uECC_vli_cmp(uECC_curve_n(C()), va, nw()) == 1 && uECC_vli_cmp(uECC_curve_n(C()), vb, nw()) == 1;
    if (ok) {
        uECC_vli_modAdd(vr, va, vb, uECC_curve_n(C()), nw());
        ok = !uECC_vli_isZero(vr, nw());
        if (ok) from_vli(out, vr);
    }
    secure_zero(va, sizeof va);
    secure_zero(vb, sizeof vb);
    secure_zero(vr, sizeof vr);
    return ok;
}

bool secp256k1_pubkey(const uint8_t priv[32], uint8_t pub[64])
{
    if (!secp256k1_scalar_valid(priv)) return false;
    return uECC_compute_public_key(priv, pub, C()) == 1;
}

bool secp256k1_pubkey_valid(const uint8_t pub[64]) { return uECC_valid_public_key(pub, C()) == 1; }

void secp256k1_compress(const uint8_t pub[64], uint8_t out[33]) { uECC_compress(pub, out, C()); }

bool secp256k1_decompress(const uint8_t in[33], uint8_t pub[64])
{
    if (in[0] != 0x02 && in[0] != 0x03) return false;
    uECC_decompress(in, pub, C());
    return secp256k1_pubkey_valid(pub);
}

/* ---- affine point addition over p (micro-ecc keeps its own internal; we need one for recovery and
 * BIP32 public derivation). Points are X||Y in native words, 2*nw words. ---- */
static bool point_add(uECC_word_t *out, const uECC_word_t *P, const uECC_word_t *Q)
{
    vli_t lam, t, x3, y3;
    const wordcount_t n = nw();
    uECC_Curve c = C();
    if (uECC_vli_equal(P, Q, n)) {
        if (!uECC_vli_equal(P + n, Q + n, n)) return false; /* P == -Q: infinity */
        /* doubling: lambda = 3x^2 / 2y  (a = 0 on secp256k1) */
        if (uECC_vli_isZero(P + n, n)) return false;
        uECC_vli_modMult_fast(t, P, P, c);
        uECC_vli_modAdd(lam, t, t, uECC_curve_p(c), n);
        uECC_vli_modAdd(lam, lam, t, uECC_curve_p(c), n);
        uECC_vli_modAdd(t, P + n, P + n, uECC_curve_p(c), n);
        uECC_vli_modInv(t, t, uECC_curve_p(c), n);
        uECC_vli_modMult_fast(lam, lam, t, c);
    } else {
        uECC_vli_modSub(t, Q, P, uECC_curve_p(c), n);
        uECC_vli_modInv(t, t, uECC_curve_p(c), n);
        uECC_vli_modSub(lam, Q + n, P + n, uECC_curve_p(c), n);
        uECC_vli_modMult_fast(lam, lam, t, c);
    }
    uECC_vli_modMult_fast(x3, lam, lam, c);
    uECC_vli_modSub(x3, x3, P, uECC_curve_p(c), n);
    uECC_vli_modSub(x3, x3, Q, uECC_curve_p(c), n);
    uECC_vli_modSub(t, P, x3, uECC_curve_p(c), n);
    uECC_vli_modMult_fast(y3, lam, t, c);
    uECC_vli_modSub(y3, y3, P + n, uECC_curve_p(c), n);
    uECC_vli_set(out, x3, n);
    uECC_vli_set(out + n, y3, n);
    secure_zero(lam, sizeof lam);
    secure_zero(t, sizeof t);
    return true;
}

static void point_from_bytes(uECC_word_t *P, const uint8_t pub[64])
{
    to_vli(P, pub);
    to_vli(P + nw(), pub + 32);
}

static void point_to_bytes(uint8_t pub[64], const uECC_word_t *P)
{
    from_vli(pub, P);
    from_vli(pub + 32, P + nw());
}

bool secp256k1_pubkey_add_scalar(const uint8_t pub[64], const uint8_t k[32], uint8_t out[64])
{
    uECC_word_t P[2 * SECP_WORDS], T[2 * SECP_WORDS], R[2 * SECP_WORDS];
    vli_t vk;
    bool ok = false;
    if (!secp256k1_pubkey_valid(pub)) return false;
    to_vli(vk, k);
    if (!scalar_ok(vk)) goto out;
    point_from_bytes(P, pub);
    uECC_point_mult(T, uECC_curve_G(C()), vk, C());
    if (uECC_valid_point(T, C()) != 1) goto out; /* the regularised ladder degenerates for a few tiny scalars */
    if (!point_add(R, P, T)) goto out;
    if (uECC_valid_point(R, C()) != 1) goto out;
    point_to_bytes(out, R);
    ok = true;
out:
    secure_zero(vk, sizeof vk);
    secure_zero(T, sizeof T);
    return ok;
}

/* ---- RFC 6979 ---- */
static void bits2octets(const uint8_t digest[32], uint8_t out[32])
{
    vli_t z;
    to_vli(z, digest);
    if (uECC_vli_cmp(uECC_curve_n(C()), z, nw()) != 1) uECC_vli_sub(z, z, uECC_curve_n(C()), nw());
    from_vli(out, z);
}

static void rfc6979_nonce(const uint8_t priv[32], const uint8_t digest[32], uint8_t k_out[32])
{
    uint8_t V[32], K[32], h1[32];
    hmac_sha256_ctx h;
    const uint8_t zero = 0, one = 1;
    bits2octets(digest, h1);
    memset(V, 0x01, 32);
    memset(K, 0x00, 32);
    hmac_sha256_init(&h, K, 32); hmac_sha256_update(&h, V, 32); hmac_sha256_update(&h, &zero, 1);
    hmac_sha256_update(&h, priv, 32); hmac_sha256_update(&h, h1, 32); hmac_sha256_final(&h, K);
    hmac_sha256(K, 32, V, 32, V);
    hmac_sha256_init(&h, K, 32); hmac_sha256_update(&h, V, 32); hmac_sha256_update(&h, &one, 1);
    hmac_sha256_update(&h, priv, 32); hmac_sha256_update(&h, h1, 32); hmac_sha256_final(&h, K);
    hmac_sha256(K, 32, V, 32, V);
    for (;;) {
        hmac_sha256(K, 32, V, 32, V); /* T = V (qlen == hlen == 256) */
        if (secp256k1_scalar_valid(V)) {
            memcpy(k_out, V, 32);
            break;
        }
        hmac_sha256_init(&h, K, 32); hmac_sha256_update(&h, V, 32); hmac_sha256_update(&h, &zero, 1);
        hmac_sha256_final(&h, K);
        hmac_sha256(K, 32, V, 32, V);
    }
    secure_zero(V, sizeof V);
    secure_zero(K, sizeof K);
    secure_zero(h1, sizeof h1);
}

static bool s_is_high(const uECC_word_t *s)
{
    vli_t half;
    uECC_vli_set(half, uECC_curve_n(C()), nw());
    uECC_vli_rshift1(half, nw());
    return uECC_vli_cmp(s, half, nw()) == 1;
}

bool secp256k1_sign(const uint8_t priv[32], const uint8_t digest[32], uint8_t sig[64], uint8_t *recid)
{
    uint8_t kb[32];
    vli_t k, d, z, r, s, t;
    uECC_word_t R[2 * SECP_WORDS];
    const uECC_word_t *n_ = uECC_curve_n(C());
    uint8_t rid;
    bool ok = false;
    if (!secp256k1_scalar_valid(priv)) return false;
    rfc6979_nonce(priv, digest, kb);
    to_vli(k, kb);
    to_vli(d, priv);
    uECC_point_mult(R, uECC_curve_G(C()), k, C());
    /* r = R.x mod n; p < 2n so one subtraction suffices */
    uECC_vli_set(r, R, nw());
    rid = 0;
    if (uECC_vli_cmp(n_, r, nw()) != 1) {
        uECC_vli_sub(r, r, n_, nw());
        rid |= 2;
    }
    if (uECC_vli_isZero(r, nw())) goto out;
    if (uECC_vli_testBit(R + nw(), 0)) rid |= 1;
    /* z = digest mod n */
    to_vli(z, digest);
    if (uECC_vli_cmp(n_, z, nw()) != 1) uECC_vli_sub(z, z, n_, nw());
    /* s = k^-1 (z + r d) mod n */
    uECC_vli_modMult(t, r, d, n_, nw());
    uECC_vli_modAdd(t, t, z, n_, nw());
    uECC_vli_modInv(k, k, n_, nw());
    uECC_vli_modMult(s, k, t, n_, nw());
    if (uECC_vli_isZero(s, nw())) goto out;
    if (s_is_high(s)) {
        uECC_vli_sub(s, n_, s, nw());
        rid ^= 1;
    }
    from_vli(sig, r);
    from_vli(sig + 32, s);
    *recid = rid;
    ok = true;
out:
    secure_zero(kb, sizeof kb);
    secure_zero(k, sizeof k);
    secure_zero(d, sizeof d);
    secure_zero(t, sizeof t);
    secure_zero(R, sizeof R);
    return ok;
}

bool secp256k1_verify(const uint8_t pub[64], const uint8_t digest[32], const uint8_t sig[64])
{
    return uECC_verify(pub, digest, 32, sig, C()) == 1;
}

bool secp256k1_is_low_s(const uint8_t sig[64])
{
    vli_t s;
    to_vli(s, sig + 32);
    return !s_is_high(s);
}

bool secp256k1_normalize_s(uint8_t sig[64])
{
    vli_t s;
    to_vli(s, sig + 32);
    if (!s_is_high(s)) return false;
    uECC_vli_sub(s, uECC_curve_n(C()), s, nw());
    from_vli(sig + 32, s);
    return true;
}

bool secp256k1_recover(const uint8_t digest[32], const uint8_t sig[64], uint8_t recid, uint8_t pub[64])
{
    vli_t x, y, t, r, s, z, rinv, u1, u2;
    uECC_word_t R[2 * SECP_WORDS], A[2 * SECP_WORDS], B[2 * SECP_WORDS], Q[2 * SECP_WORDS];
    const uECC_word_t *n_ = uECC_curve_n(C()), *p_ = uECC_curve_p(C());
    const wordcount_t n = nw();
    uECC_Curve c = C();
    if (recid > 3) return false;
    to_vli(r, sig);
    to_vli(s, sig + 32);
    if (!scalar_ok(r) || !scalar_ok(s)) return false;
    /* x = r (+ n) must be a field element */
    uECC_vli_set(x, r, n);
    if (recid & 2) {
        if (uECC_vli_add(x, x, n_, n)) return false;
        if (uECC_vli_cmp(p_, x, n) != 1) return false;
    }
    /* y^2 = x^3 + 7 */
    uECC_vli_modMult_fast(t, x, x, c);
    uECC_vli_modMult_fast(t, t, x, c);
    uECC_vli_clear(y, n);
    y[0] = 7;
    uECC_vli_modAdd(t, t, y, p_, n);
    uECC_vli_set(y, t, n);
    uECC_vli_mod_sqrt(y, c);
    uECC_vli_modMult_fast(u1, y, y, c);
    if (!uECC_vli_equal(u1, t, n)) return false; /* no square root: not a valid point */
    if ((uECC_vli_testBit(y, 0) != 0) != ((recid & 1) != 0)) uECC_vli_sub(y, p_, y, n);
    uECC_vli_set(R, x, n);
    uECC_vli_set(R + n, y, n);
    if (uECC_valid_point(R, c) != 1) return false;
    /* Q = r^-1 (s R - z G) */
    to_vli(z, digest);
    if (uECC_vli_cmp(n_, z, n) != 1) uECC_vli_sub(z, z, n_, n);
    uECC_vli_modInv(rinv, r, n_, n);
    uECC_vli_modMult(u2, s, rinv, n_, n);
    uECC_vli_modMult(u1, z, rinv, n_, n);
    if (!uECC_vli_isZero(u1, n)) uECC_vli_sub(u1, n_, u1, n); /* -z r^-1 */
    if (uECC_vli_isZero(u1, n)) {
        uECC_point_mult(Q, R, u2, c);
    } else {
        uECC_point_mult(A, uECC_curve_G(c), u1, c);
        uECC_point_mult(B, R, u2, c);
        if (uECC_valid_point(A, c) != 1 || uECC_valid_point(B, c) != 1) return false;
        if (!point_add(Q, A, B)) return false;
    }
    if (uECC_valid_point(Q, c) != 1) return false; /* micro-ecc: 1 = valid, 0 = invalid / infinity */
    point_to_bytes(pub, Q);
    return true;
}

int secp256k1_find_recid(const uint8_t pub[64], const uint8_t digest[32], const uint8_t sig[64])
{
    uint8_t cand[64];
    uint8_t rid;
    for (rid = 0; rid < 4; rid++) {
        if (secp256k1_recover(digest, sig, rid, cand) && memcmp(cand, pub, 64) == 0) return rid;
    }
    return -1;
}

/* ---- DER ---- */
static size_t der_int(const uint8_t v[32], uint8_t *out)
{
    size_t skip = 0, n;
    while (skip < 31 && v[skip] == 0) skip++;
    n = 32 - skip;
    out[0] = 0x02;
    if (v[skip] & 0x80) {
        out[1] = (uint8_t)(n + 1);
        out[2] = 0;
        memcpy(out + 3, v + skip, n);
        return 3 + n;
    }
    out[1] = (uint8_t)n;
    memcpy(out + 2, v + skip, n);
    return 2 + n;
}

size_t secp256k1_der_encode(const uint8_t sig[64], uint8_t out[SECP256K1_DER_MAX])
{
    size_t lr, ls;
    uint8_t tmp[70];
    lr = der_int(sig, tmp);
    ls = der_int(sig + 32, tmp + lr);
    out[0] = 0x30;
    out[1] = (uint8_t)(lr + ls);
    memcpy(out + 2, tmp, lr + ls);
    return 2 + lr + ls;
}

static bool der_read_int(const uint8_t *p, size_t len, size_t *used, uint8_t out[32])
{
    size_t n;
    if (len < 2 || p[0] != 0x02) return false;
    n = p[1];
    if (n == 0 || n > 33 || 2 + n > len) return false;
    if (p[2] & 0x80) return false;                       /* negative */
    if (n > 1 && p[2] == 0 && !(p[3] & 0x80)) return false; /* non-minimal */
    if (n == 33) {
        if (p[2] != 0) return false;
        memcpy(out, p + 3, 32);
    } else {
        memset(out, 0, 32);
        memcpy(out + 32 - n, p + 2, n);
    }
    *used = 2 + n;
    return true;
}

bool secp256k1_der_decode(const uint8_t *der, size_t len, uint8_t sig[64])
{
    size_t ur, us;
    if (len < 8 || len > SECP256K1_DER_MAX || der[0] != 0x30 || der[1] != len - 2) return false;
    if (!der_read_int(der + 2, len - 2, &ur, sig)) return false;
    if (!der_read_int(der + 2 + ur, len - 2 - ur, &us, sig + 32)) return false;
    return 2 + ur + us == len;
}
