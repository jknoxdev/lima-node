use aes_gcm::{
    aead::{Aead, KeyInit, Payload},
    Aes256Gcm, Key, Nonce,
};

// ── LF layout constants (mirrors lima_lf_t) ───────────────────────────────
// Full wire frame is 184B. gateway stores LF[2..184]; mfr_id carries LF[0..2].
// strips proto_version (LF[0]) and event_type (LF[1]) into the mfr_id HashMap 
// key, so the DB blob is 182B.
// We reconstruct the full 184B before slicing.

const LF_FULL_LEN:       usize = 184; // mfr_id carries LF[0..2].
const LF_STRIPPED_LEN:   usize = 182; // gateway/DB stores LF[2..184]
const LF_HEADER_LEN:     usize = 4;   // AAD: LF[0..4]
const LF_NONCE_OFFSET:   usize = 4;   // LF[4..16]
const LF_NONCE_LEN:      usize = 12;
const LF_CIPHER_OFFSET:  usize = 16;  // LF[16..104]
const LF_CIPHER_LEN:     usize = 88;
const LF_TAG_OFFSET:     usize = 104; // LF[104..120]
const LF_TAG_LEN:        usize = 16;
// outer_sig at LF[120..184] — not needed for decryption, already verified
// by the gateway before DB insert.

// ── LER layout constants (mirrors lima_ler_t) ─────────────────────────────
const LER_LEN:            usize = 24;
const LER_NODE_ID_LEN:    usize = 6;
// inner_sig follows LER in plaintext — 64B, ignored for now (v1)
const INNER_SIG_LEN:      usize = 64;
const PLAINTEXT_LEN:      usize = LER_LEN + INNER_SIG_LEN; // 88B

/// Decrypted LIMA Event Record — parsed from the 24B inner plaintext.
#[derive(Debug, Clone, PartialEq)]
pub struct Ler {
    pub node_id:      [u8; 6],
    pub event_type:   u8,
    pub sequence:     u32,
    pub timestamp_ms: u32,
    pub accel_g:      f32,
    pub delta_pa:     f32,
}

/// Reconstruct the full 184B LF from the 182B btleplug-stripped blob.
/// btleplug puts proto_version and event_type into the mfr_id key,
/// so the gateway stores the blob starting at LF[2]. We prepend
/// proto_version (0x02) and the real event_type from the DB — both
/// must match exactly what the firmware used as AAD during encryption.
/// The gateway already verified the outer sig over the original full frame.
///
/// Note: the reconstructed header bytes are used only for AAD during
/// decryption. The outer_sig was already verified by the gateway.
fn reconstruct_lf(stripped: &[u8], event_type: u8) -> anyhow::Result<[u8; LF_FULL_LEN]> {
    anyhow::ensure!(
        stripped.len() == LF_STRIPPED_LEN,
        "expected 182B stripped blob, got {}B",
        stripped.len()
    );

    let mut full = [0u8; LF_FULL_LEN];
    full[0] = 0x02; // proto_version
    full[1] = event_type; // event_type — first byte of stripped payload
    
    // Actually: btleplug strips LF[0] and LF[1] into mfr_id key.
    // So stripped blob = LF[2..184] = 182 bytes.
    full[2..].copy_from_slice(stripped);

    Ok(full)
}

/// Decrypt a raw 182B blob from the DB and return a parsed LER.
///
/// Pipeline:
///   1. Reconstruct 184B LF
///   2. Slice AAD, nonce, ciphertext+tag
///   3. AES-256-GCM decrypt → 88B plaintext
///   4. Parse first 24B as LER
pub fn decrypt_ler(raw_blob: &[u8], event_type: u8, key: &[u8; 32]) -> anyhow::Result<Ler> {
    // 1. Reconstruct full LF
    let lf = reconstruct_lf(raw_blob, event_type)?;

    // 2. Slice fields per spec
    let aad        = &lf[..LF_HEADER_LEN];                               // [0..4]
    let nonce_bytes= &lf[LF_NONCE_OFFSET..LF_NONCE_OFFSET + LF_NONCE_LEN]; // [4..16]
    let ciphertext = &lf[LF_CIPHER_OFFSET..LF_CIPHER_OFFSET + LF_CIPHER_LEN]; // [16..104]
    let tag        = &lf[LF_TAG_OFFSET..LF_TAG_OFFSET + LF_TAG_LEN];     // [104..120]

    // aes-gcm expects ciphertext || tag as a single buffer
    let mut ct_with_tag = Vec::with_capacity(LF_CIPHER_LEN + LF_TAG_LEN);
    ct_with_tag.extend_from_slice(ciphertext);
    ct_with_tag.extend_from_slice(tag);

    // 3. AES-256-GCM decrypt
    let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(key));
    let nonce  = Nonce::from_slice(nonce_bytes);

    let plaintext = cipher
        .decrypt(nonce, Payload { msg: &ct_with_tag, aad })
        .map_err(|_| anyhow::anyhow!("AES-256-GCM decrypt failed — wrong key or corrupted frame"))?;

    anyhow::ensure!(
        plaintext.len() == PLAINTEXT_LEN,
        "unexpected plaintext length: expected {}B, got {}B",
        PLAINTEXT_LEN,
        plaintext.len()
    );

    // 4. Parse LER from first 24B — inner_sig (remaining 64B) ignored for v1
    parse_ler(&plaintext[..LER_LEN])
}

/// Parse a 24B slice into a LER struct.
/// All multi-byte fields are little-endian per spec.
fn parse_ler(bytes: &[u8]) -> anyhow::Result<Ler> {
    anyhow::ensure!(bytes.len() == LER_LEN, "LER must be 24 bytes");

    let mut node_id = [0u8; LER_NODE_ID_LEN];
    node_id.copy_from_slice(&bytes[0..6]);

    let event_type   = bytes[6];
    // bytes[7] = reserved, skip
    let sequence     = u32::from_le_bytes(bytes[8..12].try_into().unwrap());
    let timestamp_ms = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    let accel_g      = f32::from_le_bytes(bytes[16..20].try_into().unwrap());
    let delta_pa     = f32::from_le_bytes(bytes[20..24].try_into().unwrap());

    Ok(Ler { node_id, event_type, sequence, timestamp_ms, accel_g, delta_pa })
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use aes_gcm::{
        aead::{Aead, KeyInit, Payload},
        Aes256Gcm, Key, Nonce,
    };

    /// Build a valid 182B stripped blob by encrypting a known LER.
    /// Returns (blob, key, original_ler) so tests can verify round-trips.
    fn make_test_blob(ler: &Ler) -> (Vec<u8>, [u8; 32]) {
        let key_bytes = [0x42u8; 32];
        let nonce_bytes = [0x01u8; 12];

        // Encode LER into 24B
        let mut ler_bytes = [0u8; LER_LEN];
        ler_bytes[0..6].copy_from_slice(&ler.node_id);
        ler_bytes[6] = ler.event_type;
        ler_bytes[7] = 0x00; // reserved
        ler_bytes[8..12].copy_from_slice(&ler.sequence.to_le_bytes());
        ler_bytes[12..16].copy_from_slice(&ler.timestamp_ms.to_le_bytes());
        ler_bytes[16..20].copy_from_slice(&ler.accel_g.to_le_bytes());
        ler_bytes[20..24].copy_from_slice(&ler.delta_pa.to_le_bytes());

        // Plaintext = LER (24B) || inner_sig (64B zeros for test)
        let mut plaintext = [0u8; PLAINTEXT_LEN];
        plaintext[..LER_LEN].copy_from_slice(&ler_bytes);

        // Build header (AAD) — this is LF[0..4] in the full frame
        // Full frame: [proto_version=0x02, event_type, reserved[2], ...]
        let aad = [0x02u8, ler.event_type, 0x00, 0x00];

        // Encrypt
        let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&key_bytes));
        let nonce  = Nonce::from_slice(&nonce_bytes);
        let ct_with_tag = cipher
            .encrypt(nonce, Payload { msg: &plaintext, aad: &aad })
            .unwrap();

        // ct_with_tag = ciphertext(88B) || tag(16B) = 104B
        let ciphertext = &ct_with_tag[..LF_CIPHER_LEN];
        let tag        = &ct_with_tag[LF_CIPHER_LEN..];

        // Build full 184B LF
        let mut lf = [0u8; LF_FULL_LEN];
        lf[0] = 0x02;
        lf[1] = ler.event_type;
        lf[2..4].copy_from_slice(&[0x00, 0x00]); // reserved
        lf[4..16].copy_from_slice(&nonce_bytes);
        lf[16..104].copy_from_slice(ciphertext);
        lf[104..120].copy_from_slice(tag);
        // outer_sig at [120..184] left as zeros — not needed for decrypt

        // btleplug strips LF[0] and LF[1], so DB blob = LF[2..184]
        let blob = lf[2..].to_vec(); // 182B

        (blob, key_bytes)
    }

    fn test_ler() -> Ler {
        Ler {
            node_id:      [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF],
            event_type:   0x01,
            sequence:     42,
            timestamp_ms: 1_700_000_000,
            accel_g:      1.23,
            delta_pa:     -0.45,
        }
    }

    // ── reconstruct_lf ───────────────────────────────────────────────────────

    #[test]
    fn test_reconstruct_lf_correct_length() {
        let stripped = vec![0u8; LF_STRIPPED_LEN];
        let full = reconstruct_lf(&stripped, 0x01).unwrap();
        assert_eq!(full.len(), LF_FULL_LEN);
    }

    #[test]
    fn test_reconstruct_lf_proto_version() {
        let stripped = vec![0u8; LF_STRIPPED_LEN];
        let full = reconstruct_lf(&stripped, 0x01).unwrap();
        assert_eq!(full[0], 0x02);
    }

    #[test]
    fn test_reconstruct_lf_rejects_wrong_length() {
        let short = vec![0u8; 100];
        assert!(reconstruct_lf(&short, 0x01).is_err());

        let long = vec![0u8; 200];
        assert!(reconstruct_lf(&long, 0x01).is_err());
    }

    #[test]
    fn test_reconstruct_lf_preserves_payload() {
        let mut stripped = vec![0u8; LF_STRIPPED_LEN];
        stripped[2] = 0xAB; // first nonce byte (offset 2 in stripped = LF[4])

        let full = reconstruct_lf(&stripped, 0x05).unwrap();
        assert_eq!(full[1], 0x05); // event_type passed as param lands at LF[1]
        assert_eq!(full[4], 0xAB); // nonce[0] at LF[4]
    }

    // ── parse_ler ─────────────────────────────────────────────────────────────

    #[test]
    fn test_parse_ler_round_trip() {
        let original = test_ler();
        let mut bytes = [0u8; LER_LEN];
        bytes[0..6].copy_from_slice(&original.node_id);
        bytes[6] = original.event_type;
        bytes[7] = 0x00;
        bytes[8..12].copy_from_slice(&original.sequence.to_le_bytes());
        bytes[12..16].copy_from_slice(&original.timestamp_ms.to_le_bytes());
        bytes[16..20].copy_from_slice(&original.accel_g.to_le_bytes());
        bytes[20..24].copy_from_slice(&original.delta_pa.to_le_bytes());

        let parsed = parse_ler(&bytes).unwrap();
        assert_eq!(parsed, original);
    }

    #[test]
    fn test_parse_ler_rejects_wrong_length() {
        assert!(parse_ler(&[0u8; 23]).is_err());
        assert!(parse_ler(&[0u8; 25]).is_err());
    }

    #[test]
    fn test_parse_ler_little_endian_sequence() {
        let mut bytes = [0u8; LER_LEN];
        // sequence = 0x01020304 in LE = [04, 03, 02, 01]
        bytes[8..12].copy_from_slice(&[0x04, 0x03, 0x02, 0x01]);
        let parsed = parse_ler(&bytes).unwrap();
        assert_eq!(parsed.sequence, 0x01020304);
    }

    // ── decrypt_ler ───────────────────────────────────────────────────────────

    #[test]
    fn test_decrypt_ler_round_trip() {
        let original = test_ler();
        let (blob, key) = make_test_blob(&original);

        let decrypted = decrypt_ler(&blob, 0x01, &key).unwrap();
        assert_eq!(decrypted, original);
    }

    #[test]
    fn test_decrypt_ler_wrong_key_fails() {
        let (blob, _) = make_test_blob(&test_ler());
        let wrong_key = [0xFFu8; 32];

        let result = decrypt_ler(&blob, 0x01, &wrong_key);
        assert!(result.is_err());
    }

    #[test]
    fn test_decrypt_ler_corrupted_ciphertext_fails() {
        let (mut blob, key) = make_test_blob(&test_ler());
        // Flip a byte in the ciphertext region (LF[16..104] → stripped[14..102])
        blob[20] ^= 0xFF;

        let result = decrypt_ler(&blob, 0x01, &key);
        assert!(result.is_err());
    }

    #[test]
    fn test_decrypt_ler_corrupted_tag_fails() {
        let (mut blob, key) = make_test_blob(&test_ler());
        // Flip a byte in the GCM tag region (LF[104..120] → stripped[102..118])
        blob[110] ^= 0xFF;

        let result = decrypt_ler(&blob, 0x01, &key);
        assert!(result.is_err());
    }

    #[test]
    fn test_decrypt_ler_wrong_blob_length_fails() {
        let key = [0x42u8; 32];
        assert!(decrypt_ler(&[0u8; 100], 0x01, &key).is_err());
        assert!(decrypt_ler(&[0u8; 184], 0x01, &key).is_err()); // full LF, not stripped
    }
}
