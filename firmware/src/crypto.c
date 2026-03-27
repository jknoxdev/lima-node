/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * crypto.c — PSA ECDSA-P256 signing + AES-256-GCM encryption module
 *
 * Key storage : Nordic KMU via persistent PSA key slots
 * Inner sign  : ECDSA-P256/SHA-256 over lima_ler_t (24B)
 * Encrypt     : AES-256-GCM over (LER || inner_sig) (88B plaintext)
 * Outer sign  : ECDSA-P256/SHA-256 over LF[0..120] (120B)
 *
 * All three operations are CryptoCell-310 hardware-accelerated.
 * Wire format spec: docs/architecture/frame-record-spec.md
 *
 * Call order:
 *   1. lima_crypto_init()        — once, before fsm_init()
 *   2. lima_crypto_build_ler()   — populate LER from fsm.last_event
 *   3. lima_crypto_sign_async()  — inner sign LER
 *   4. lima_crypto_build_lf()    — encrypt + outer sign → LF
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <psa/crypto.h>
#include <string.h>
#include "crypto.h"
#include "events.h"

LOG_MODULE_REGISTER(lima_crypto, CONFIG_LIMA_CRYPTO_LOG_LEVEL);

/* ── Private state ───────────────────────────────────────────────────────── */

static uint32_t     sequence_counter = 0;
static psa_key_id_t signing_key_id   = PSA_KEY_ID_NULL;
static psa_key_id_t aes_key_id       = PSA_KEY_ID_NULL;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static psa_status_t provision_ecdsa_key(psa_key_id_t *out_key_id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attr, CONFIG_LIMA_CRYPTO_KEY_ID);
    psa_set_key_type(&attr,
        PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr,
        PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);

    psa_status_t status = psa_generate_key(&attr, out_key_id);
    psa_reset_key_attributes(&attr);

    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: ECDSA key generation failed (%d)", status);
        return status;
    }

    LOG_INF("CRYPTO: ECDSA-P256 keypair generated (persistent, id=0x%08X)",
            *out_key_id);
    return PSA_SUCCESS;
}

/**
 * @brief Generate AES-256 PSK, persist to KMU, and log for operator capture.
 *
 * Key is generated on-device and exported to serial log in hex.
 * Operator captures the hex string from serial output and stores
 * in Bitwarden secure note for use by the web client.
 *
 * Key is generated with EXPORT flag to allow one-time log export.
 * In production, remove EXPORT flag after provisioning is confirmed.
 */
static psa_status_t provision_aes_key(psa_key_id_t *out_key_id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attr, CONFIG_LIMA_CRYPTO_AES_KEY_ID);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_usage_flags(&attr,
        PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_EXPORT);

    psa_status_t status = psa_generate_key(&attr, out_key_id);
    psa_reset_key_attributes(&attr);

    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: AES key generation failed (%d)", status);
        return status;
    }

    /* Export and log for operator capture — store in Bitwarden secure note */
    uint8_t key_bytes[32];
    size_t  key_len;

    status = psa_export_key(*out_key_id, key_bytes, sizeof(key_bytes), &key_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: AES key export failed (%d)", status);
        return status;
    }

    LOG_INF("CRYPTO: ══════════════════════════════════════════════════");
    LOG_INF("CRYPTO: AES-256 PSK generated — CAPTURE AND STORE NOW");
    LOG_INF("CRYPTO: Store in Bitwarden secure note: 'LIMA AES Key'");
    LOG_HEXDUMP_INF(key_bytes, key_len, "  AES-256 PSK:");
    LOG_INF("CRYPTO: ══════════════════════════════════════════════════");

    /* Zero key material from stack immediately after logging */
    memset(key_bytes, 0, sizeof(key_bytes));

    LOG_INF("CRYPTO: AES-256 PSK stored (persistent, id=0x%08X)", *out_key_id);
    return PSA_SUCCESS;
}

static void log_public_key(psa_key_id_t key_id)
{
    uint8_t pub[65];
    size_t  pub_len;

    psa_status_t status = psa_export_public_key(key_id, pub, sizeof(pub),
                                                &pub_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: public key export failed (%d)", status);
        return;
    }

    LOG_INF("CRYPTO: ECDSA public key (%u bytes) — register with gateway:", pub_len);
    LOG_HEXDUMP_INF(pub, pub_len, "  pubkey:");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int lima_crypto_init(void)
{
    psa_status_t status;

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: psa_crypto_init failed (%d)", status);
        return -EIO;
    }

    /* ── ECDSA signing key ───────────────────────────────────────────────── */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    status = psa_get_key_attributes(CONFIG_LIMA_CRYPTO_KEY_ID, &attr);
    psa_reset_key_attributes(&attr);

    if (status == PSA_SUCCESS) {
        LOG_INF("CRYPTO: ECDSA key found at slot 0x%08X",
                CONFIG_LIMA_CRYPTO_KEY_ID);
        signing_key_id = CONFIG_LIMA_CRYPTO_KEY_ID;
    } else if (status == PSA_ERROR_INVALID_HANDLE ||
               status == PSA_ERROR_DOES_NOT_EXIST) {
        LOG_INF("CRYPTO: no ECDSA key at slot 0x%08X — provisioning",
                CONFIG_LIMA_CRYPTO_KEY_ID);
        status = provision_ecdsa_key(&signing_key_id);
        if (status != PSA_SUCCESS) {
            return -EIO;
        }
    } else {
        LOG_ERR("CRYPTO: ECDSA key query failed (%d)", status);
        return -EIO;
    }

    log_public_key(signing_key_id);

    /* ── AES-256 encryption key ──────────────────────────────────────────── */
    psa_key_attributes_t aes_attr = PSA_KEY_ATTRIBUTES_INIT;
    status = psa_get_key_attributes(CONFIG_LIMA_CRYPTO_AES_KEY_ID, &aes_attr);
    psa_reset_key_attributes(&aes_attr);

    if (status == PSA_SUCCESS) {
        LOG_INF("CRYPTO: AES-256 key found at slot 0x%08X",
                CONFIG_LIMA_CRYPTO_AES_KEY_ID);
        aes_key_id = CONFIG_LIMA_CRYPTO_AES_KEY_ID;
    } else if (status == PSA_ERROR_INVALID_HANDLE ||
               status == PSA_ERROR_DOES_NOT_EXIST) {
        LOG_INF("CRYPTO: no AES-256 key at slot 0x%08X — provisioning",
                CONFIG_LIMA_CRYPTO_AES_KEY_ID);
        status = provision_aes_key(&aes_key_id);
        if (status != PSA_SUCCESS) {
            return -EIO;
        }
    } else {
        LOG_ERR("CRYPTO: AES key query failed (%d)", status);
        return -EIO;
    }

    LOG_INF("CRYPTO: initialized — ECDSA-P256 + AES-256-GCM ready");
    LOG_INF("CRYPTO:   signing key:    0x%08X", signing_key_id);
    LOG_INF("CRYPTO:   encryption key: 0x%08X", aes_key_id);
    return 0;
}

void lima_crypto_build_ler(lima_ler_t *ler, const lima_event_t *evt)
{
    __ASSERT_NO_MSG(ler != NULL);
    __ASSERT_NO_MSG(evt != NULL);

    memset(ler, 0, sizeof(*ler));

    bt_addr_le_t addr;
    size_t count = 1;
    bt_id_get(&addr, &count);
    memcpy(ler->node_id, addr.a.val, sizeof(ler->node_id));

    ler->sequence     = ++sequence_counter;
    ler->timestamp_ms = evt->timestamp_ms;
    ler->event_type   = (uint8_t)evt->type;

    switch (evt->type) {
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
        ler->accel_g  = evt->data.imu.accel_g;
        break;
    case LIMA_EVT_PRESSURE_BREACH:
        ler->delta_pa = evt->data.baro.delta_pa;
        break;
    case LIMA_EVT_TAMPER_DETECTED:
        break;
    default:
        break;
    }

    LOG_INF("CRYPTO: LER built — node=%02X:%02X:%02X:%02X:%02X:%02X "
            "seq=%u evt=0x%02X accel=%.2f delta_pa=%.2f",
            ler->node_id[0], ler->node_id[1], ler->node_id[2],
            ler->node_id[3], ler->node_id[4], ler->node_id[5],
            ler->sequence, ler->event_type,
            (double)ler->accel_g, (double)ler->delta_pa);
    LOG_HEXDUMP_INF((const uint8_t *)ler, sizeof(lima_ler_t), "  LER:");
}

int lima_crypto_sign_async(const lima_ler_t *ler, lima_sign_cb_t cb)
{
    if (ler == NULL || cb == NULL) {
        LOG_ERR("CRYPTO: NULL parameter");
        return -EINVAL;
    }
    if (signing_key_id == PSA_KEY_ID_NULL) {
        LOG_ERR("CRYPTO: not initialized");
        return -ECANCELED;
    }

    lima_sig_result_t result = { 0 };

    result.err = (int)psa_sign_message(
        signing_key_id,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256),
        (const uint8_t *)ler,
        sizeof(lima_ler_t),
        result.sig,
        sizeof(result.sig),
        &result.sig_len
    );

    if (result.err != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: psa_sign_message failed (%d)", result.err);
    } else {
        LOG_INF("CRYPTO: LER inner-signed — sig[0..3]=%02X%02X%02X%02X (%u bytes)",
                result.sig[0], result.sig[1], result.sig[2], result.sig[3],
                result.sig_len);
        LOG_HEXDUMP_INF(result.sig, result.sig_len, "  inner_sig:");
    }

    cb(&result);
    return 0;
}

int lima_crypto_build_lf(const lima_ler_t *ler,
                          const uint8_t *inner_sig,
                          size_t inner_sig_len,
                          lima_lf_t *out_lf)
{
    if (ler == NULL || inner_sig == NULL || out_lf == NULL) {
        LOG_ERR("CRYPTO: build_lf NULL parameter");
        return -EINVAL;
    }
    if (inner_sig_len != LIMA_LF_OUTER_SIG_LEN) {
        LOG_ERR("CRYPTO: inner_sig_len %u != 64", inner_sig_len);
        return -EINVAL;
    }
    if (aes_key_id == PSA_KEY_ID_NULL) {
        LOG_ERR("CRYPTO: AES key not initialized");
        return -ECANCELED;
    }
    if (signing_key_id == PSA_KEY_ID_NULL) {
        LOG_ERR("CRYPTO: signing key not initialized");
        return -ECANCELED;
    }

    psa_status_t status;

    /* ── Step 1: Zero the LF and fill header ────────────────────────────── */
    memset(out_lf, 0, sizeof(*out_lf));
    out_lf->proto_version = 0x02;
    out_lf->event_type    = ler->event_type;
    /* reserved stays zero */

    /* ── Step 2: Generate random nonce ─────────────────────────────────── */
    status = psa_generate_random(out_lf->nonce, sizeof(out_lf->nonce));
    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: nonce generation failed (%d)", status);
        return -EIO;
    }
    LOG_INF("CRYPTO: nonce[0..3]=%02X%02X%02X%02X",
            out_lf->nonce[0], out_lf->nonce[1],
            out_lf->nonce[2], out_lf->nonce[3]);

    /* ── Step 3: AES-256-GCM encrypt (LER || inner_sig) ─────────────────── */
    /*
     * Plaintext:  LER (24B) || inner_sig (64B) = 88B
     * AAD:        LF header (4B) — authenticated but not encrypted
     * Output:     ciphertext (88B) || gcm_tag (16B) = 104B total from PSA
     *
     * psa_aead_encrypt() appends the tag at the end of the ciphertext buffer.
     * We split it: first 88B → lf.ciphertext, last 16B → lf.gcm_tag.
     */
    uint8_t plaintext[LIMA_LF_PLAINTEXT_LEN];
    memcpy(plaintext,                        ler,       sizeof(lima_ler_t));
    memcpy(plaintext + sizeof(lima_ler_t),   inner_sig, inner_sig_len);

    /* PSA output buffer: ciphertext (88B) + tag (16B) */
    uint8_t aead_out[LIMA_LF_CIPHERTEXT_LEN + LIMA_LF_TAG_LEN];
    size_t  aead_out_len = 0;

    /* AAD = LF header bytes [0..3] */
    const uint8_t *aad     = (const uint8_t *)out_lf;
    size_t         aad_len = LIMA_LF_HEADER_LEN;

    status = psa_aead_encrypt(
        aes_key_id,
        PSA_ALG_GCM,
        out_lf->nonce,  sizeof(out_lf->nonce),
        aad,            aad_len,
        plaintext,      sizeof(plaintext),
        aead_out,       sizeof(aead_out),
        &aead_out_len
    );

    /* Zero plaintext from stack immediately */
    memset(plaintext, 0, sizeof(plaintext));

    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: psa_aead_encrypt failed (%d)", status);
        return -EIO;
    }

    if (aead_out_len != LIMA_LF_CIPHERTEXT_LEN + LIMA_LF_TAG_LEN) {
        LOG_ERR("CRYPTO: unexpected aead output len %u", aead_out_len);
        return -EIO;
    }

    /* Split PSA output into ciphertext and gcm_tag fields */
    memcpy(out_lf->ciphertext, aead_out,                       LIMA_LF_CIPHERTEXT_LEN);
    memcpy(out_lf->gcm_tag,    aead_out + LIMA_LF_CIPHERTEXT_LEN, LIMA_LF_TAG_LEN);

    LOG_INF("CRYPTO: AES-256-GCM encrypt done — "
            "ciphertext[0..3]=%02X%02X%02X%02X tag[0..3]=%02X%02X%02X%02X",
            out_lf->ciphertext[0], out_lf->ciphertext[1],
            out_lf->ciphertext[2], out_lf->ciphertext[3],
            out_lf->gcm_tag[0],    out_lf->gcm_tag[1],
            out_lf->gcm_tag[2],    out_lf->gcm_tag[3]);

    /* ── Step 4: ECDSA-P256 outer sign over LF[0..120] ──────────────────── */
    /*
     * Signed region: proto_version(1) + event_type(1) + reserved(2) +
     *                nonce(12) + ciphertext(88) + gcm_tag(16) = 120B
     * Covers everything except outer_sig itself.
     */
    size_t outer_sig_len = 0;

    status = psa_sign_message(
        signing_key_id,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256),
        (const uint8_t *)out_lf,
        LIMA_LF_SIGNED_LEN,
        out_lf->outer_sig,
        sizeof(out_lf->outer_sig),
        &outer_sig_len
    );

    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: outer psa_sign_message failed (%d)", status);
        return -EIO;
    }

    LOG_INF("CRYPTO: LF outer-signed — outer_sig[0..3]=%02X%02X%02X%02X (%u bytes)",
            out_lf->outer_sig[0], out_lf->outer_sig[1],
            out_lf->outer_sig[2], out_lf->outer_sig[3],
            outer_sig_len);
    LOG_HEXDUMP_INF((const uint8_t *)out_lf, sizeof(lima_lf_t), "  LF:");

    LOG_INF("CRYPTO: LF assembled — 184B encrypted+signed ✓");
    return 0;
}
