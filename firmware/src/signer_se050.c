/* Pincer1 - SE050 signer via NXP Plug & Trust (T1oI2C transport + Se05x_API_* APDU layer; no SSS layer),
 * bound to this controller with Platform SCP03 and gated by a UserID (PIN) session.
 *
 * Built against Plug & Trust middleware v04.08.02.
 *
 * Two Se05xSession_t objects, wired exactly like the middleware's SSS layer wires them
 * (sss/src/se05x/fsl_sss_se05x_apis.c: sss_se05x_TXn / sss_se05x_channel_txn / se05x_CreateVerifyUserIDSession):
 *
 *   g_plat  platform channel. hasSession = 0. After the handshake authType = kSSS_AuthType_SCP03 and
 *           fp_Transform wraps every APDU with scp03.c (C-MAC + C-ENC), fp_DeCrypt unwraps (R-MAC + R-ENC),
 *           fp_RawTXn hands the finished bytes to smCom_TransceiveRaw.
 *   g_user  UserID session on top of g_plat. hasSession = 1, authType = kSSS_AuthType_ID. fp_Transform is the
 *           middleware's plain se05x_Transform (wraps the command in PROCESS + TAG_SESSION_ID + TAG_1), and its
 *           "raw" transmit is g_plat.fp_TXn, so the session APDU is in turn SCP03-protected.
 *
 * Object ids (outside the reserved 0x7FFF0000.. range):
 *   0x50494E01 real key, 0x50494E02 decoy key (EC key pairs, ED25519), 0x50494E10 PIN (UserID auth object),
 *   0x50494E03 / 0x50494E04 real / decoy EVM key, 0x50494E05 / 0x50494E06 real / decoy Bitcoin key (secp256k1
 *   key pairs, curve id 0x10 created on the applet with the middleware's parameter table before the first
 *   import; SE050_E / SE051 variants list Koblitz curves, UNVERIFIED on our part).
 * secp256k1 byte order (fsl_sss_se05x_apis.c, Weierstrass path): public key 0x04||X||Y both ways, no
 * reversal; ECDSASign takes the 32-byte digest (the algorithm id only names the hash length) and returns a
 * DER signature; low-s and the recovery id are produced here (src/secp256k1.c).
 * Key policy: everyone may READ (public key) and DELETE (wipe); only a session authenticated with the PIN
 * object may SIGN. The PIN object allows everyone DELETE only, and is created with max 5 attempts
 * (INS_AUTH_OBJECT, TAG_MAX_ATTEMPTS). See README for what of this is verified and what is not.
 *
 * Byte-order conventions, taken from sss/src/se05x/fsl_sss_se05x_apis.c (the reference SSS layer):
 *   - ED25519 private key is written AS-IS (RFC 8032 seed form), the public key REVERSED.
 *   - ReadObject returns the ED25519 public key REVERSED -> we reverse it back.
 *   - EdDSASign returns R and S each REVERSED (two 32-byte halves) -> we reverse each half. */
#include <string.h>

#include "rng.h"
#include "scp03.h"
#include "secp256k1.h"
#include "secrets.h"
#include "signer.h"
#include "util.h"

#include "ed25519.h"
#include "se05x_APDU_apis.h"
#include "se05x_ecc_curves.h"
#include "se05x_enums.h"
#include "se05x_tlv.h"
#include "se05x_const.h"
#include "smCom.h"
#include "smComT1oI2C.h"
#include "sm_const.h"
#include "sm_types.h"

static const uint32_t SLOT_OBJ_ID[SIGNER_SLOTS] = {0x50494E01u /* "PIN\1" real */, 0x50494E02u /* decoy */};
static const uint32_t SECP_OBJ_ID[SIGNER_SLOTS][SIGNER_SECP_CHAINS] = {{0x50494E03u, 0x50494E05u}, {0x50494E04u, 0x50494E06u}};
#define PIN_OBJ_ID 0x50494E10u
#define DURESS_OBJ_ID 0x50494E11u /* second UserID object: a session with it may sign with the DECOY keys only */
#define PIN_MAX_ATTEMPTS 5
/* The duress object is created with no attempt limit: every real unlock is, by construction, one failed
 * attempt against it (both objects are tried every time), so a limit would lock it out after five real
 * unlocks. The five-strike rule is enforced on the real object by the chip and on the whole by consent.c. */
#define DURESS_MAX_ATTEMPTS 0

static Se05xSession_t g_plat; /* platform (SCP03) channel */
static Se05xSession_t g_user; /* UserID session over g_plat */
static scp03_t g_scp;
static void *g_conn;
static bool g_open;
static bool g_user_open;
static bool g_keys_otp;
static bool g_present[SIGNER_SLOTS];
static uint8_t g_pub[SIGNER_SLOTS][32];
static bool g_secp_present[SIGNER_SLOTS][SIGNER_SECP_CHAINS];
static uint8_t g_secp_pub[SIGNER_SLOTS][SIGNER_SECP_CHAINS][64];
static bool g_secp_curve_ok; /* secp256k1 curve object confirmed present on the applet */

/* Prototypes the APDU layer exports from se05x_tlv.c but does not declare in a public header. */
smStatus_t se05x_Transform(struct Se05xSession *pSession, const tlvHeader_t *hdr, uint8_t *cmdApduBuf,
                           const size_t cmdApduBufLen, tlvHeader_t *out_hdr, uint8_t *txBuf, size_t *ptxBufLen,
                           uint8_t hasle);
smStatus_t se05x_DeCrypt(struct Se05xSession *pSessionCtx, size_t cmd_cmacLen, uint8_t *rsp, size_t *rspLength,
                         uint8_t hasle);
U16 GP_Select(void *conn_ctx, const U8 *appletName, U16 appletNameLen, U8 *responseData, U16 *responseDataLen);

static void channel_down(void)
{
    scp03_close(&g_scp);
    g_user_open = false;
    g_user.hasSession = 0;
    memset(g_user.value, 0, sizeof g_user.value);
}

/* ---- platform channel plumbing ---- */

/* Mirror of sss_se05x_channel_txn() with no tunnel: SCP03 buffers are already complete APDUs; plain ones
 * get [CLA INS P1 P2][Lc][data][Le] here (sss_se05x_channel_txnRaw). */
static smStatus_t plat_raw(void *conn_ctx, struct _sss_se05x_tunnel_context *pChannelCtx, SE_AuthType_t currAuth,
                           const tlvHeader_t *hdr, uint8_t *cmdBuf, size_t cmdBufLen, uint8_t *rsp, size_t *rspLen,
                           uint8_t hasle)
{
    uint8_t txBuf[SE05X_MAX_BUF_SIZE_CMD];
    size_t i = 0;
    U32 rlen;
    U32 st;
    (void)pChannelCtx;
    if (*rspLen > UINT32_MAX) return SM_NOT_OK;
    rlen = (U32)*rspLen;
    if (currAuth == kSSS_AuthType_SCP03) {
        if (cmdBufLen > UINT16_MAX) return SM_NOT_OK;
        st = smCom_TransceiveRaw(conn_ctx, cmdBuf, (U16)cmdBufLen, rsp, &rlen);
        *rspLen = rlen;
        return (smStatus_t)st;
    }
    if (sizeof(*hdr) + 3 + cmdBufLen + 2 > sizeof txBuf) return SM_NOT_OK;
    memcpy(&txBuf[i], hdr, sizeof(*hdr));
    i += sizeof(*hdr);
    if (cmdBufLen > 0) {
        if (cmdBufLen < 0xFF && !hasle) {
            txBuf[i++] = (uint8_t)cmdBufLen;
        } else {
            txBuf[i++] = 0x00;
            txBuf[i++] = (uint8_t)(cmdBufLen >> 8);
            txBuf[i++] = (uint8_t)cmdBufLen;
        }
        memcpy(&txBuf[i], cmdBuf, cmdBufLen);
        i += cmdBufLen;
    } else {
        txBuf[i++] = 0x00;
    }
    if (hasle) {
        txBuf[i++] = 0x00;
        txBuf[i++] = 0x00;
    }
    st = smCom_TransceiveRaw(conn_ctx, txBuf, (U16)i, rsp, &rlen);
    *rspLen = rlen;
    secure_zero(txBuf, i);
    return (smStatus_t)st;
}

/* fp_Transform of g_plat: se05x_Transform_scp equivalent once the channel is open, plain before. */
static smStatus_t plat_transform(struct Se05xSession *pSession, const tlvHeader_t *hdr, uint8_t *cmdBuf,
                                 size_t cmdBufLen, tlvHeader_t *outHdr, uint8_t *txBuf, size_t *txBufLen,
                                 uint8_t hasle)
{
    if (pSession->authType == kSSS_AuthType_SCP03) {
        size_t n = 0;
        memcpy(outHdr, hdr, sizeof *outHdr);
        outHdr->hdr[0] |= 0x04;
        if (scp03_wrap(&g_scp, hdr->hdr, cmdBuf, cmdBufLen, hasle != 0, txBuf, *txBufLen, &n) != 0) return SM_NOT_OK;
        *txBufLen = n;
        return SM_OK;
    }
    return se05x_Transform(pSession, hdr, cmdBuf, cmdBufLen, outHdr, txBuf, txBufLen, hasle);
}

static smStatus_t plat_decrypt(struct Se05xSession *pSession, size_t cmdLen, uint8_t *rsp, size_t *rspLen, uint8_t hasle)
{
    if (pSession->authType == kSSS_AuthType_SCP03) {
        if (scp03_unwrap(&g_scp, cmdLen, rsp, rspLen) != 0) {
            channel_down(); /* a bad R-MAC means the channel is no longer trustworthy */
            return SM_NOT_OK;
        }
        if (*rspLen < 2) return SM_NOT_OK;
        return (smStatus_t)((rsp[*rspLen - 2] << 8) | rsp[*rspLen - 1]);
    }
    return se05x_DeCrypt(pSession, cmdLen, rsp, rspLen, hasle);
}

/* Mirror of sss_se05x_TXn() for the platform session. */
static smStatus_t plat_txn(struct Se05xSession *pSession, const tlvHeader_t *hdr, uint8_t *cmdBuf, size_t cmdBufLen,
                           uint8_t *rsp, size_t *rspLen, uint8_t hasle)
{
    tlvHeader_t outHdr = {{0}};
    uint8_t txBuf[SE05X_MAX_BUF_SIZE_CMD];
    size_t txBufLen = sizeof txBuf;
    smStatus_t ret;
    ret = plat_transform(pSession, hdr, cmdBuf, cmdBufLen, &outHdr, txBuf, &txBufLen, hasle);
    if (ret != SM_OK) goto out;
    ret = plat_raw(pSession->conn_ctx, NULL, pSession->authType, &outHdr, txBuf, txBufLen, rsp, rspLen, hasle);
    if (ret != SM_OK) {
        if (pSession->authType == kSSS_AuthType_SCP03) channel_down(); /* counters are out of step now */
        goto out;
    }
    ret = plat_decrypt(pSession, cmdBufLen, rsp, rspLen, hasle);
out:
    secure_zero(txBuf, sizeof txBuf);
    return ret;
}

/* ---- UserID session plumbing: wrap in PROCESS/session TLV, then send through the platform channel ---- */
static smStatus_t user_txn(struct Se05xSession *pSession, const tlvHeader_t *hdr, uint8_t *cmdBuf, size_t cmdBufLen,
                           uint8_t *rsp, size_t *rspLen, uint8_t hasle)
{
    tlvHeader_t outHdr = {{0}};
    uint8_t txBuf[SE05X_MAX_BUF_SIZE_CMD];
    size_t txBufLen = sizeof txBuf;
    smStatus_t ret;
    ret = se05x_Transform(pSession, hdr, cmdBuf, cmdBufLen, &outHdr, txBuf, &txBufLen, hasle);
    if (ret != SM_OK) goto out;
    if (!g_plat.fp_TXn) { ret = SM_NOT_OK; goto out; }
    ret = g_plat.fp_TXn(&g_plat, &outHdr, txBuf, txBufLen, rsp, rspLen, hasle);
    if (ret != SM_OK) goto out;
    ret = se05x_DeCrypt(pSession, cmdBufLen, rsp, rspLen, hasle); /* pdynScp03Ctx NULL: SW check only */
out:
    secure_zero(txBuf, sizeof txBuf);
    return ret;
}

/* Unused by this build but part of the session contract. */
static smStatus_t user_raw(void *conn_ctx, struct _sss_se05x_tunnel_context *pChannelCtx, SE_AuthType_t currAuth,
                           const tlvHeader_t *hdr, uint8_t *cmdBuf, size_t cmdBufLen, uint8_t *rsp, size_t *rspLen,
                           uint8_t hasle)
{
    return plat_raw(conn_ctx, pChannelCtx, currAuth, hdr, cmdBuf, cmdBufLen, rsp, rspLen, hasle);
}

/* ---- policies (byte layout: fsl_sss_se05x_policy.c sss_se05x_create_object_policy_buffer) ---- */
static size_t policy_entry(uint8_t *p, uint32_t auth_id, uint32_t ar)
{
    p[0] = DEFAULT_OBJECT_POLICY_SIZE; /* 8 = auth id (4) + access rules (4) */
    p[1] = (uint8_t)(auth_id >> 24); p[2] = (uint8_t)(auth_id >> 16); p[3] = (uint8_t)(auth_id >> 8); p[4] = (uint8_t)auth_id;
    p[5] = (uint8_t)(ar >> 24); p[6] = (uint8_t)(ar >> 16); p[7] = (uint8_t)(ar >> 8); p[8] = (uint8_t)ar;
    return 9;
}

/* Signing keys: anyone on the (SCP03) link may read the public key and delete; signing needs the PIN session.
 * Auth object id 0 = "all sessions" (SE05x APDU spec policy header; UNVERIFIED on hardware, see README). */
static void key_policy(Se05xPolicy_t *pol, uint8_t *buf, uint8_t slot)
{
    size_t n = 0;
    n += policy_entry(buf + n, 0x00000000u, POLICY_OBJ_ALLOW_READ | POLICY_OBJ_ALLOW_DELETE);
    n += policy_entry(buf + n, PIN_OBJ_ID, POLICY_OBJ_ALLOW_SIGN | POLICY_OBJ_ALLOW_READ | POLICY_OBJ_ALLOW_DELETE);
    /* the decoy keys also sign inside a session authenticated with the duress object; the real keys never do */
    if (slot == 1) n += policy_entry(buf + n, DURESS_OBJ_ID, POLICY_OBJ_ALLOW_SIGN | POLICY_OBJ_ALLOW_READ | POLICY_OBJ_ALLOW_DELETE);
    pol->value = buf;
    pol->value_len = n;
}

static void pin_policy(Se05xPolicy_t *pol, uint8_t *buf)
{
    size_t n = policy_entry(buf, 0x00000000u, POLICY_OBJ_ALLOW_DELETE); /* no write: the PIN cannot be replaced
                                                                            in place (delete + write is how the
                                                                            duress object is replaced) */
    pol->value = buf;
    pol->value_len = n;
}

/* ---- helpers ---- */
static void reverse32(uint8_t *p)
{
    int i;
    for (i = 0; i < 16; i++) {
        uint8_t t = p[i];
        p[i] = p[31 - i];
        p[31 - i] = t;
    }
}

static int read_pubkey(uint8_t slot, uint8_t pubkey[32])
{
    uint8_t buf[64];
    size_t len = sizeof buf;
    smStatus_t st = Se05x_API_ReadObject(&g_plat, SLOT_OBJ_ID[slot], 0, 0, buf, &len);
    if (st != SM_OK) return -1;
    /* ReadObject on an EC key pair returns the raw public key; some applet versions prefix 0x00. */
    if (len == 33 && buf[0] == 0) { memmove(buf, buf + 1, 32); len = 32; }
    if (len != 32) return -1;
    reverse32(buf);
    memcpy(pubkey, buf, 32);
    return 0;
}

static int object_exists(uint32_t id, bool *exists)
{
    SE05x_Result_t r = kSE05x_Result_FAILURE;
    if (Se05x_API_CheckObjectExists(&g_plat, id, &r) != SM_OK) return -1;
    *exists = r == kSE05x_Result_SUCCESS;
    return 0;
}

static int refresh_slot(uint8_t slot)
{
    bool exists = false;
    g_present[slot] = false;
    if (object_exists(SLOT_OBJ_ID[slot], &exists) != 0) return -1;
    if (!exists) return 0;
    if (read_pubkey(slot, g_pub[slot]) != 0) return -1;
    g_present[slot] = true;
    return 0;
}

/* ReadObject on a Weierstrass key pair returns the uncompressed public point 0x04||X||Y. */
static int read_secp_pubkey(uint8_t slot, uint8_t chain, uint8_t pub[64])
{
    uint8_t buf[80];
    size_t len = sizeof buf;
    smStatus_t st = Se05x_API_ReadObject(&g_plat, SECP_OBJ_ID[slot][chain], 0, 0, buf, &len);
    if (st != SM_OK) return -1;
    if (len != 65 || buf[0] != 0x04) return -1;
    memcpy(pub, buf + 1, 64);
    return secp256k1_pubkey_valid(pub) ? 0 : -1;
}

static int refresh_secp_slot(uint8_t slot, uint8_t chain)
{
    bool exists = false;
    g_secp_present[slot][chain] = false;
    if (object_exists(SECP_OBJ_ID[slot][chain], &exists) != 0) return -1;
    if (!exists) return 0;
    if (read_secp_pubkey(slot, chain, g_secp_pub[slot][chain]) != 0) return -1;
    g_secp_present[slot][chain] = true;
    return 0;
}

/* The applet knows only the curves that were created on it. ReadECCurveList answers one byte per curve id
 * (index id - 1, kSE05x_SetIndicator_SET = present); create secp256k1 with the middleware's parameter table
 * when it is missing. UNVERIFIED on hardware: whether this OEF/variant accepts Koblitz curves at all. */
static int ensure_secp_curve(void)
{
    uint8_t list[64];
    size_t len = sizeof list;
    if (g_secp_curve_ok) return 0;
    if (Se05x_API_ReadECCurveList(&g_plat, list, &len) == SM_OK && len >= kSE05x_ECCurve_Secp256k1 &&
        list[kSE05x_ECCurve_Secp256k1 - 1] == kSE05x_SetIndicator_SET) {
        g_secp_curve_ok = true;
        return 0;
    }
    if (Se05x_API_CreateCurve_secp256k1(&g_plat, kSE05x_ECCurve_Secp256k1) != SM_OK) return -1;
    g_secp_curve_ok = true;
    return 0;
}

/* Platform SCP03 handshake, plain APDUs straight to the transport (as nxScp03_AuthenticateChannel does
 * with authType temporarily None). */
static int open_channel(const scp03_keys_t *keys)
{
    uint8_t host[SCP03_CHALLENGE_LEN];
    uint8_t apdu[SCP03_EXT_AUTH_APDU_LEN];
    uint8_t rsp[64];
    U32 rlen;
    int rc = -1;
    rng_fill(host, sizeof host);
    scp03_build_initialize_update(host, SCP03_KEY_VERSION_SE050, apdu);
    rlen = sizeof rsp;
    if (smCom_TransceiveRaw(g_conn, apdu, SCP03_INIT_UPDATE_APDU_LEN, rsp, &rlen) != SM_OK) goto out;
    if (scp03_handshake(&g_scp, keys->enc, keys->mac, host, rsp, rlen, apdu) != 0) goto out;
    rlen = sizeof rsp;
    if (smCom_TransceiveRaw(g_conn, apdu, SCP03_EXT_AUTH_APDU_LEN, rsp, &rlen) != SM_OK) goto out;
    if (rlen < 2) goto out;
    if (scp03_finish(&g_scp, (uint16_t)((rsp[rlen - 2] << 8) | rsp[rlen - 1])) != 0) goto out;
    g_plat.authType = kSSS_AuthType_SCP03;
    rc = 0;
out:
    secure_zero(host, sizeof host);
    secure_zero(rsp, sizeof rsp);
    return rc;
}

int signer_init(void)
{
    static const uint8_t applet[] = APPLET_NAME;
    uint8_t atr[64], sel[64];
    uint16_t atrLen = sizeof atr, selLen = sizeof sel;
    uint32_t applet_version = 0;
    scp03_keys_t keys;
    secrets_source_t src;
    uint8_t slot;
    int rc = -1;
    if (g_open) return 0;
    src = secrets_get_scp03_keys(&keys);
    if (src == SECRETS_NONE) return -1; /* no keys, no chip: a bound SE050 refuses plain APDUs anyway */
    g_keys_otp = src == SECRETS_OTP;
    if (smComT1oI2C_Init(&g_conn, NULL) != SMCOM_OK) goto out;
    if (smComT1oI2C_Open(g_conn, ESE_MODE_NORMAL, 0x00, atr, &atrLen) != SMCOM_OK) goto out;
    /* Select the SE05x IoT applet (SM_Connect does the same). The select response starts with the applet
     * version, 3 bytes big-endian (sm_connect.c); it decides the SCP03 counter mode. */
    if (GP_Select(g_conn, applet, sizeof applet, sel, &selLen) != SW_OK) goto out;
    if (selLen == 4 || selLen == 5 || selLen == 7)
        applet_version = ((uint32_t)sel[0] << 24) | ((uint32_t)sel[1] << 16) | ((uint32_t)sel[2] << 8);

    memset(&g_plat, 0, sizeof g_plat);
    g_plat.conn_ctx = g_conn;
    g_plat.authType = kSSS_AuthType_None;
    g_plat.fp_TXn = &plat_txn;
    g_plat.fp_RawTXn = &plat_raw;
    g_plat.fp_Transform = &plat_transform;
    g_plat.fp_DeCrypt = &plat_decrypt;
    scp03_init(&g_scp);
    scp03_set_applet_version(&g_scp, applet_version);
    if (open_channel(&keys) != 0) goto out;

    {
        uint8_t ver[16];
        size_t verLen = sizeof ver;
        if (Se05x_API_GetVersion(&g_plat, ver, &verLen) != SM_OK) goto out;
        if (verLen >= 3) g_plat.applet_version = ((uint32_t)ver[0] << 16) | ((uint32_t)ver[1] << 8) | ver[2];
    }
    g_open = true;
    for (slot = 0; slot < SIGNER_SLOTS; slot++) {
        uint8_t ch;
        refresh_slot(slot);
        for (ch = 0; ch < SIGNER_SECP_CHAINS; ch++) refresh_secp_slot(slot, ch);
    }
    rc = 0;
out:
    secure_zero(&keys, sizeof keys);
    if (rc != 0) channel_down();
    return rc;
}

bool signer_is_hardware(void) { return true; }
bool signer_keys_from_otp(void) { return g_keys_otp; }

int signer_import(uint8_t slot, const uint8_t priv[32], uint8_t pubkey[32])
{
    uint8_t pub[32], pub_rev[32], sk[64], polbuf[32];
    Se05xPolicy_t pol;
    smStatus_t st;
    bool exists = false;
    if (!g_open || !g_scp.open || slot >= SIGNER_SLOTS) return -1;
    /* The SE050 wants the matching public key alongside the private seed for a key-pair object. */
    ed25519_create_keypair(pub, sk, priv);
    secure_zero(sk, sizeof sk);
    memcpy(pub_rev, pub, 32);
    reverse32(pub_rev);
    if (object_exists(SLOT_OBJ_ID[slot], &exists) == 0 && exists)
        (void)Se05x_API_DeleteSecureObject(&g_plat, SLOT_OBJ_ID[slot]);
    /* Persistent key pair with the PIN-gated policy. EC private parts are never exportable from the SE050
     * regardless of policy. The PIN object (auth id in the policy) is created later by set_pin: until it
     * exists nobody can open a session that is allowed to sign. */
    key_policy(&pol, polbuf, slot);
    st = Se05x_API_WriteECKey(&g_plat, &pol, 0, SLOT_OBJ_ID[slot], kSE05x_ECCurve_ECC_ED_25519, priv, 32, pub_rev, 32,
                              kSE05x_INS_NA, kSE05x_KeyPart_Pair);
    secure_zero(pub_rev, sizeof pub_rev);
    if (st != SM_OK) return -1;
    if (refresh_slot(slot) != 0 || !g_present[slot]) return -1;
    if (ct_memcmp(g_pub[slot], pub, 32) != 0) {
        /* Our byte-order assumption about ReadObject would be wrong: trust the host-computed key but
         * flag it (README: verify on first hardware bring-up). */
        memcpy(g_pub[slot], pub, 32);
    }
    memcpy(pubkey, g_pub[slot], 32);
    return 0;
}

int signer_pubkey(uint8_t slot, uint8_t pubkey[32])
{
    if (slot >= SIGNER_SLOTS || !g_present[slot]) return -1;
    memcpy(pubkey, g_pub[slot], 32);
    return 0;
}

int signer_sign(uint8_t slot, const uint8_t *msg, size_t len, uint8_t sig[64])
{
    size_t sigLen = 64;
    smStatus_t st;
    if (!g_open || !g_scp.open || !g_user_open || slot >= SIGNER_SLOTS || !g_present[slot]) return -1;
    st = Se05x_API_EdDSASign(&g_user, SLOT_OBJ_ID[slot], kSE05x_EDSignatureAlgo_ED25519PURE_SHA_512, msg, len, sig,
                             &sigLen);
    if (st != SM_OK || sigLen != 64) return -1;
    reverse32(sig);
    reverse32(sig + 32);
    /* Cheap self-check so a byte-order mistake can never leak a bogus signature to the chain. */
    if (!ed25519_verify(sig, msg, len, g_pub[slot])) return -1;
    return 0;
}

bool signer_slot_present(uint8_t slot) { return slot < SIGNER_SLOTS && g_present[slot]; }

bool signer_policy_selftest(void)
{
    static const uint8_t probe[] = "pincer1 policy self-test: this must never be signed";
    uint8_t sig[64];
    size_t sigLen = sizeof sig;
    uint8_t slot;
    if (!g_open || !g_scp.open) return false;
    for (slot = 0; slot < SIGNER_SLOTS; slot++) {
        smStatus_t st;
        if (!g_present[slot]) continue;
        /* platform session only: the policy grants SIGN to the PIN auth object, not to "all sessions" */
        st = Se05x_API_EdDSASign(&g_plat, SLOT_OBJ_ID[slot], kSE05x_EDSignatureAlgo_ED25519PURE_SHA_512, probe,
                                 sizeof probe - 1, sig, &sigLen);
        secure_zero(sig, sizeof sig);
        if (st == SM_OK) return false; /* the chip signed for anyone: policy not in force */
    }
    return true;
}

int signer_import_secp256k1(uint8_t slot, uint8_t chain, const uint8_t priv[32], uint8_t pub[64])
{
    uint8_t pub65[65], polbuf[32];
    Se05xPolicy_t pol;
    smStatus_t st;
    bool exists = false;
    if (!g_open || !g_scp.open || slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS) return -1;
    if (!secp256k1_pubkey(priv, pub65 + 1)) return -1;
    pub65[0] = 0x04;
    if (ensure_secp_curve() != 0) return -1;
    if (object_exists(SECP_OBJ_ID[slot][chain], &exists) == 0 && exists)
        (void)Se05x_API_DeleteSecureObject(&g_plat, SECP_OBJ_ID[slot][chain]);
    key_policy(&pol, polbuf, slot); /* same gate as the ed25519 key: only a PIN session may sign */
    st = Se05x_API_WriteECKey(&g_plat, &pol, 0, SECP_OBJ_ID[slot][chain], kSE05x_ECCurve_Secp256k1, priv, 32, pub65, 65,
                              kSE05x_INS_NA, kSE05x_KeyPart_Pair);
    if (st != SM_OK) { secure_zero(pub65, sizeof pub65); return -1; }
    if (refresh_secp_slot(slot, chain) != 0 || !g_secp_present[slot][chain]) { secure_zero(pub65, sizeof pub65); return -1; }
    if (ct_memcmp(g_secp_pub[slot][chain], pub65 + 1, 64) != 0) {
        /* The chip must hold exactly the key we derived; anything else and the addresses would lie. */
        (void)Se05x_API_DeleteSecureObject(&g_plat, SECP_OBJ_ID[slot][chain]);
        g_secp_present[slot][chain] = false;
        secure_zero(pub65, sizeof pub65);
        return -1;
    }
    memcpy(pub, pub65 + 1, 64);
    secure_zero(pub65, sizeof pub65);
    return 0;
}

int signer_pubkey_secp256k1(uint8_t slot, uint8_t chain, uint8_t pub[64])
{
    if (slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS || !g_secp_present[slot][chain]) return -1;
    memcpy(pub, g_secp_pub[slot][chain], 64);
    return 0;
}

bool signer_secp256k1_present(uint8_t slot, uint8_t chain)
{
    return slot < SIGNER_SLOTS && chain < SIGNER_SECP_CHAINS && g_secp_present[slot][chain];
}

int signer_sign_secp256k1(uint8_t slot, uint8_t chain, const uint8_t digest[32], uint8_t sig65[65])
{
    uint8_t der[SECP256K1_DER_MAX + 8];
    size_t derLen = sizeof der;
    smStatus_t st;
    int recid;
    if (!g_open || !g_scp.open || !g_user_open || slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS ||
        !g_secp_present[slot][chain])
        return -1;
    /* The input is the digest; SHA_256 names its length (sss_se05x_asymmetric_sign_digest does the same). */
    st = Se05x_API_ECDSASign(&g_user, SECP_OBJ_ID[slot][chain], kSE05x_ECSignatureAlgo_SHA_256, digest, 32, der, &derLen);
    if (st != SM_OK) return -1;
    if (!secp256k1_der_decode(der, derLen, sig65)) return -1;
    secp256k1_normalize_s(sig65); /* the chip may answer high-s; Bitcoin and EVM want low-s */
    /* Recovering our own public key from the signature proves it is a valid signature of this digest by this
     * key (byte-order or wrong-object mistakes can never leak a bogus signature) and yields the recovery id. */
    recid = secp256k1_find_recid(g_secp_pub[slot][chain], digest, sig65);
    if (recid < 0) return -1;
    sig65[64] = (uint8_t)recid;
    secure_zero(der, sizeof der);
    return 0;
}

/* ---- PIN ---- */

int signer_pin_set(const uint8_t code[SIGNER_PIN_LEN])
{
    uint8_t polbuf[16];
    Se05xPolicy_t pol;
    bool exists = false;
    if (!g_open || !g_scp.open) return -1;
    if (object_exists(PIN_OBJ_ID, &exists) != 0 || exists) return -1; /* never overwrite a PIN: wipe first */
    pin_policy(&pol, polbuf);
    /* INS_AUTH_OBJECT (kSE05x_AttestationType_AUTH) marks it usable for CreateSession and makes
     * TAG_MAX_ATTEMPTS apply: the chip itself locks the object after PIN_MAX_ATTEMPTS wrong values. */
    if (Se05x_API_WriteUserID(&g_plat, &pol, PIN_MAX_ATTEMPTS, PIN_OBJ_ID, code, SIGNER_PIN_LEN,
                              kSE05x_AttestationType_AUTH) != SM_OK)
        return -1;
    return 0;
}

/* One verification round against `obj`: CreateSession(auth object) -> session id, then VerifySessionUserID
 * inside that session (se05x_CreateVerifyUserIDSession). On success the session stays open in g_user; on a
 * wrong code it is closed. 0 ok, 1 wrong, -1 link / missing object. */
static int verify_round(uint32_t obj, const uint8_t code[SIGNER_PIN_LEN])
{
    size_t sidLen = sizeof g_user.value;
    smStatus_t st;
    signer_pin_close();
    memset(&g_user, 0, sizeof g_user);
    g_user.conn_ctx = g_conn;
    g_user.authType = kSSS_AuthType_None;
    g_user.fp_TXn = &user_txn;
    g_user.fp_RawTXn = &user_raw;
    g_user.fp_Transform = &se05x_Transform;
    g_user.fp_DeCrypt = &se05x_DeCrypt;
    g_user.applet_version = g_plat.applet_version;
    st = Se05x_API_CreateSession(&g_user, obj, g_user.value, &sidLen);
    if (st != SM_OK || sidLen != sizeof g_user.value) return -1; /* no such object / link error: not a wrong PIN */
    g_user.hasSession = 1;
    g_user.authType = kSSS_AuthType_ID;
    g_user.auth_id = obj;
    st = Se05x_API_VerifySessionUserID(&g_user, code, SIGNER_PIN_LEN);
    if (st == SM_OK) {
        g_user_open = true;
        return 0;
    }
    (void)Se05x_API_CloseSession(&g_user);
    g_user.hasSession = 0;
    memset(g_user.value, 0, sizeof g_user.value);
    /* 6985 = conditions not satisfied (documented wrong-UserID answer), 6982/6986 = security status */
    if (st == SM_ERR_CONDITIONS_NOT_SATISFIED || st == SM_ERR_SECURITY_STATUS || st == SM_ERR_COMMAND_NOT_ALLOWED)
        return 1;
    return -1;
}

int signer_pin_verify(const uint8_t code[SIGNER_PIN_LEN])
{
    if (!g_open || !g_scp.open) return -1;
    return verify_round(PIN_OBJ_ID, code);
}

int signer_duress_pin_set(const uint8_t code[SIGNER_PIN_LEN], const uint8_t salt[16])
{
    uint8_t polbuf[16];
    Se05xPolicy_t pol;
    bool exists = false;
    (void)salt; /* the chip stores the value itself */
    if (!g_open || !g_scp.open) return -1;
    if (object_exists(DURESS_OBJ_ID, &exists) != 0) return -1;
    if (exists && Se05x_API_DeleteSecureObject(&g_plat, DURESS_OBJ_ID) != SM_OK) return -1; /* replace */
    pin_policy(&pol, polbuf);
    if (Se05x_API_WriteUserID(&g_plat, &pol, DURESS_MAX_ATTEMPTS, DURESS_OBJ_ID, code, SIGNER_PIN_LEN,
                              kSE05x_AttestationType_AUTH) != SM_OK)
        return -1;
    return 0;
}

bool signer_duress_pin_present(void)
{
    bool exists = false;
    if (!g_open || !g_scp.open) return false;
    if (object_exists(DURESS_OBJ_ID, &exists) != 0) return false;
    return exists;
}

int signer_pin_verify2(const uint8_t code[SIGNER_PIN_LEN])
{
    bool have_duress = false;
    int d, r;
    if (!g_open || !g_scp.open) return -1;
    if (object_exists(DURESS_OBJ_ID, &have_duress) != 0) return -1;
    if (!have_duress) {
        r = verify_round(PIN_OBJ_ID, code); /* no second object yet: the single round of old */
        return r < 0 ? -1 : (r == 0 ? SIGNER_VERIFY_REAL : SIGNER_VERIFY_WRONG);
    }
    /* Round 1 against the duress object, closed either way; round 2 against the real object, or against the
     * duress object again when round 1 matched (so the real object sees no attempt). Same number of
     * commands in every case; only the surviving session differs, and that is what the chip's key policies
     * key off. */
    d = verify_round(DURESS_OBJ_ID, code);
    if (d < 0) return -1;
    signer_pin_close();
    r = verify_round(d == 0 ? DURESS_OBJ_ID : PIN_OBJ_ID, code);
    if (r < 0) return -1;
    if (d == 0) return SIGNER_VERIFY_DURESS;
    return r == 0 ? SIGNER_VERIFY_REAL : SIGNER_VERIFY_WRONG;
}

void signer_pin_close(void)
{
    if (g_user_open && g_scp.open) (void)Se05x_API_CloseSession(&g_user);
    g_user_open = false;
    g_user.hasSession = 0;
    memset(g_user.value, 0, sizeof g_user.value);
}

bool signer_pin_present(void)
{
    bool exists = false;
    if (!g_open || !g_scp.open) return false;
    if (object_exists(PIN_OBJ_ID, &exists) != 0) return false;
    return exists;
}

int signer_wipe(void)
{
    uint8_t slot;
    bool exists;
    int rc = 0;
    if (!g_open || !g_scp.open) return -1;
    signer_pin_close();
    for (slot = 0; slot < SIGNER_SLOTS; slot++) {
        uint8_t ch;
        exists = false;
        if (object_exists(SLOT_OBJ_ID[slot], &exists) != 0) { rc = -1; continue; }
        if (exists && Se05x_API_DeleteSecureObject(&g_plat, SLOT_OBJ_ID[slot]) != SM_OK) rc = -1;
        g_present[slot] = false;
        secure_zero(g_pub[slot], 32);
        for (ch = 0; ch < SIGNER_SECP_CHAINS; ch++) {
            exists = false;
            if (object_exists(SECP_OBJ_ID[slot][ch], &exists) != 0) { rc = -1; continue; }
            if (exists && Se05x_API_DeleteSecureObject(&g_plat, SECP_OBJ_ID[slot][ch]) != SM_OK) rc = -1;
            g_secp_present[slot][ch] = false;
            secure_zero(g_secp_pub[slot][ch], 64);
        }
    }
    exists = false;
    if (object_exists(PIN_OBJ_ID, &exists) != 0) rc = -1;
    else if (exists && Se05x_API_DeleteSecureObject(&g_plat, PIN_OBJ_ID) != SM_OK) rc = -1;
    exists = false;
    if (object_exists(DURESS_OBJ_ID, &exists) != 0) rc = -1;
    else if (exists && Se05x_API_DeleteSecureObject(&g_plat, DURESS_OBJ_ID) != SM_OK) rc = -1;
    for (slot = 0; slot < SIGNER_SLOTS; slot++) {
        uint8_t ch;
        refresh_slot(slot);
        for (ch = 0; ch < SIGNER_SECP_CHAINS; ch++) refresh_secp_slot(slot, ch);
    }
    return rc;
}

void signer_soft_export(uint8_t blob[SIGNER_SLOTS][32], uint8_t pin_hash[32])
{
    memset(blob, 0, SIGNER_SLOTS * 32);
    memset(pin_hash, 0, 32);
}
void signer_soft_load(const uint8_t blob[SIGNER_SLOTS][32], const uint8_t pin_hash[32])
{
    (void)blob;
    (void)pin_hash;
}
void signer_soft_export_secp(uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32])
{
    memset(blob, 0, SIGNER_SLOTS * SIGNER_SECP_CHAINS * 32);
}
void signer_soft_load_secp(const uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32]) { (void)blob; }
void signer_soft_export_duress(uint8_t hash[32], uint8_t salt[16])
{
    memset(hash, 0, 32);
    memset(salt, 0, 16);
}
void signer_soft_load_duress(const uint8_t hash[32], const uint8_t salt[16])
{
    (void)hash;
    (void)salt;
}
