/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * crypto.c — PSA ECDSA-P256 / SHA-256 signing module
 *
 * Key storage : Nordic KMU via persistent PSA key slot
 * Algorithm   : ECDSA over NIST P-256 with SHA-256
 * Payload     : lima_payload_t (24 bytes), signed in full
 *
 * Call order (from main.c and fsm.c):
 *   1. lima_crypto_init()            — once, before fsm_init()
 *   2. lima_crypto_build_payload()   — populate from fsm.last_event
 *   3. lima_crypto_sign_async()      — sign, cb posts SIGNING_COMPLETE
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
// #include <zephyr/bluetooth/bluetooth.h>
#include <psa/crypto.h>
#include <string.h>
#include "crypto.h"
#include "events.h"

LOG_MODULE_REGISTER(lima_crypto, CONFIG_LIMA_CRYPTO_LOG_LEVEL);

/* ── Private state ───────────────────────────────────────────────────────── */

/* Monotonic sequence counter — incremented on every signed event.
 * TODO(v2): persist to flash via NVS so counter survives power cycles. */
static uint32_t sequence_counter = 0;

/* PSA key handle — opened once in lima_crypto_init(), reused for all signs */
static psa_key_id_t signing_key_id = PSA_KEY_ID_NULL;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * @brief Generate and persist a P-256 keypair into KMU slot.
 *
 * Called only when no persistent key exists at CONFIG_LIMA_CRYPTO_KEY_ID.
 * In production, replace with factory provisioning via nrfutil KMU push.
 *
 * @return PSA_SUCCESS on success, PSA error code on failure.
 */
static psa_status_t provision_key(psa_key_id_t *out_key_id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

    // psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attr, CONFIG_LIMA_CRYPTO_KEY_ID); // persistent only

    psa_set_key_type(&attr,
        PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr,
        PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);

    // psa_key_id_t key_id; // unused in new persistent code
    // psa_status_t status = psa_generate_key(&attr, &key_id);
    psa_status_t status = psa_generate_key(&attr, out_key_id);

    psa_reset_key_attributes(&attr);

    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: key generation failed (%d)", status);
        return status;
    }

    // LOG_INF("CRYPTO: P-256 keypair generated and stored in KMU slot 0x%08X",
    //         CONFIG_LIMA_CRYPTO_KEY_ID);
    // LOG_INF("CRYPTO: P-256 keypair generated (volatile, id=0x%08X)", *out_key_id);
    LOG_INF("CRYPTO: P-256 keypair generated (persistent, id=0x%08X)", *out_key_id);
    return PSA_SUCCESS;
}

/**
 * @brief Export and log the public key bytes for gateway registration.
 *
 * Logs 65 bytes (uncompressed point: 0x04 || X || Y) at INF level.
 * In production, write to flash or transmit via provisioning channel.
 *
 * @param key_id  PSA key ID of the persistent signing key.
 */
static void log_public_key(psa_key_id_t key_id)
{
    uint8_t  pub[65];   /* uncompressed P-256 point: 0x04 || X[32] || Y[32] */
    size_t   pub_len;

    psa_status_t status = psa_export_public_key(key_id, pub, sizeof(pub),
                                                &pub_len);
    if (status != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: public key export failed (%d)", status);
        return;
    }

    LOG_INF("CRYPTO: public key (%u bytes):", pub_len);
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

    /* Check if persistent key already exists */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    status = psa_get_key_attributes(CONFIG_LIMA_CRYPTO_KEY_ID, &attr);
    psa_reset_key_attributes(&attr);

    if (status == PSA_SUCCESS) {
        /* Key exists — load it */
        LOG_INF("CRYPTO: existing key found at slot 0x%08X",
                CONFIG_LIMA_CRYPTO_KEY_ID);
        signing_key_id = CONFIG_LIMA_CRYPTO_KEY_ID;
    } else if (status == PSA_ERROR_INVALID_HANDLE ||
               status == PSA_ERROR_DOES_NOT_EXIST) {
        /* First boot — generate and persist */
        LOG_INF("CRYPTO: no key at slot 0x%08X — provisioning",
                CONFIG_LIMA_CRYPTO_KEY_ID);
        status = provision_key(&signing_key_id);
        if (status != PSA_SUCCESS) {
            LOG_ERR("CRYPTO: provision_key failed (%d)", status);
            return -EIO;
        }
    } else {
        LOG_ERR("CRYPTO: key attribute query failed (%d)", status);
        return -EIO;
    }

    log_public_key(signing_key_id);
    LOG_INF("CRYPTO: initialized — ECDSA-P256/SHA-256 ready (key_id=0x%08X)",
            signing_key_id);
    return 0;
}

void lima_crypto_build_payload(lima_payload_t *payload,
                               const lima_event_t *evt)
{
    __ASSERT_NO_MSG(payload != NULL);
    __ASSERT_NO_MSG(evt != NULL);

    memset(payload, 0, sizeof(*payload));

    /* node_id — pull BLE MAC from controller FICR */
    // bt_addr_le_t addr;
    // size_t count = 1;
    // bt_id_get(&addr, &count);
    // memcpy(payload->node_id, addr.a.val, sizeof(payload->node_id));

    /* TODO(v2): replace with bt_id_get() when BLE stack is enabled */
    static const uint8_t dev_node_id[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    memcpy(payload->node_id, dev_node_id, sizeof(payload->node_id));

    /* Monotonic sequence counter — anti-replay */
    payload->sequence     = ++sequence_counter;

    /* Event metadata */
    payload->timestamp_ms = evt->timestamp_ms;
    payload->event_type   = (uint8_t)evt->type;

    /* Sensor data — copy from whichever union arm is relevant.
     * For non-sensor events, these will be zero from the memset above. */
    switch (evt->type) {
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
        payload->accel_g  = evt->data.imu.accel_g;
        break;

    case LIMA_EVT_PRESSURE_BREACH:
        payload->delta_pa = evt->data.baro.delta_pa;
        break;

    case LIMA_EVT_TAMPER_DETECTED:
        /* Tamper has no sensor data — accel_g and delta_pa stay zero */
        break;

    default:
        break;
    }

    /* reserved[2] stays zero from memset */

    LOG_INF("CRYPTO: payload built — node=%02X:%02X:%02X:%02X:%02X:%02X "
            "seq=%u evt=0x%02X accel=%.2f delta_pa=%.2f",
            payload->node_id[0], payload->node_id[1], payload->node_id[2],
            payload->node_id[3], payload->node_id[4], payload->node_id[5],
            payload->sequence,
            payload->event_type,
            (double)payload->accel_g,
            (double)payload->delta_pa);
    LOG_HEXDUMP_INF((const uint8_t *)payload, sizeof(lima_payload_t), "  payload:");
}

int lima_crypto_sign_async(const lima_payload_t *payload,
                           lima_sign_cb_t cb)
{
    if (payload == NULL || cb == NULL) {
        LOG_ERR("CRYPTO: NULL parameter");
        return -EINVAL;
    }

    if (signing_key_id == PSA_KEY_ID_NULL) {
        LOG_ERR("CRYPTO: not initialized — call lima_crypto_init() first");
        return -ECANCELED;
    }

    lima_sig_result_t result = { 0 };

    /* Sign the full 24-byte payload struct.
     * psa_sign_message() hashes internally with SHA-256 before signing. */
    result.err = (int)psa_sign_message(
        signing_key_id,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256),
        (const uint8_t *)payload,
        sizeof(lima_payload_t),
        result.sig,
        sizeof(result.sig),
        &result.sig_len
    );

    if (result.err != PSA_SUCCESS) {
        LOG_ERR("CRYPTO: psa_sign_message failed (%d)", result.err);
    } else {
        LOG_INF("CRYPTO: signed %u bytes — sig[0..3]=%02X%02X%02X%02X",
                result.sig_len,
                result.sig[0], result.sig[1],
                result.sig[2], result.sig[3]);
        LOG_INF("CRYPTO: full signature (%u bytes):", result.sig_len);
        LOG_HEXDUMP_INF(result.sig, result.sig_len, "  sig:");
        LOG_INF("CRYPTO: signed payload (%u bytes):", sizeof(lima_payload_t));
        LOG_HEXDUMP_INF((const uint8_t *)payload, sizeof(lima_payload_t), "  payload:");
  
    }

    /* Invoke callback — cb() posts event to FSM queue */
    cb(&result);

    return 0;
}