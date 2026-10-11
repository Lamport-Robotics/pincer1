// Scalar → public key on secp224r1 with mbedTLS. Compiled for the ESP32 targets always, and on
// the host only with -DFINDMY_HOST_MBEDTLS (test/test_findmy then checks a vector from the Python helper).
#if defined(ARDUINO) || defined(FINDMY_HOST_MBEDTLS)
#include "findmy.h"
#include <string.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

#ifdef ARDUINO
#include <esp_system.h>
static int rng_cb(void *, unsigned char *out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}
#else
#include <stdlib.h>
static int rng_cb(void *, unsigned char *out, size_t len) {   // blinding only; not a secret source
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)rand();
    return 0;
}
#endif

int findmy_public_key(const uint8_t d[FINDMY_KEY_LEN], uint8_t pub_x[FINDMY_KEY_LEN]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point P;
    mbedtls_mpi m;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&P);
    mbedtls_mpi_init(&m);
    int ok = 0;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP224R1) == 0 &&
        mbedtls_mpi_read_binary(&m, d, FINDMY_KEY_LEN) == 0 &&
        mbedtls_ecp_check_privkey(&grp, &m) == 0 &&
        mbedtls_ecp_mul(&grp, &P, &m, &grp.G, rng_cb, NULL) == 0) {
        unsigned char buf[1 + 2 * FINDMY_KEY_LEN];   // 0x04 ‖ X ‖ Y
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &P, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, buf, sizeof buf) == 0 &&
            olen == sizeof buf) {
            memcpy(pub_x, buf + 1, FINDMY_KEY_LEN);
            ok = 1;
        }
    }
    mbedtls_mpi_free(&m);
    mbedtls_ecp_point_free(&P);
    mbedtls_ecp_group_free(&grp);
    return ok;
}
#endif
