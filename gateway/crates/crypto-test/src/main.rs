//! LIMA crypto-test — end-to-end round-trip verification
//!
//! Simulates the full node → gateway crypto pipeline:
//!
//!   1. Generate ephemeral node keypair  (simulates nRF52840 first-boot)
//!   2. Generate ephemeral gateway keypair
//!   3. ECDH → shared secret
//!   4. HKDF-SHA256 → AES-256-GCM key
//!   5. Build fake LimaPayload
//!   6. ECDSA-P256 sign payload (inner sig)
//!   7. AES-256-GCM encrypt (payload || inner_sig)
//!   8. ECDSA-P256 sign (nonce || ciphertext || tag) (outer sig)
//!   ── wire boundary ──
//!   9. Verify outer sig
//!  10. AES-256-GCM decrypt
//!  11. Verify inner sig
//!  12. Assert plaintext == original payload
//!  13. Hex dump every step

use aes_gcm::{
    aead::{Aead, KeyInit, OsRng as AesOsRng},
    Aes256Gcm, Nonce,
};
use hkdf::Hkdf;
use lima_types::{
    LimaEventType, LimaPayload, HKDF_INFO, INNER_SIG_LEN, NONCE_LEN,
    PAYLOAD_LEN, TAG_LEN,
};
use p256::{
    ecdh::EphemeralSecret,
    ecdsa::{signature::Signer, signature::Verifier, Signature, SigningKey, VerifyingKey},
    PublicKey,
};
use rand_core::OsRng;
use sha2::Sha256;

// ── Helpers ───────────────────────────────────────────────────────────────────

fn hex_dump(label: &str, data: &[u8]) {
    println!("\n┌─ {} ({} bytes)", label, data.len());
    for (i, chunk) in data.chunks(16).enumerate() {
        let hex: Vec<String> = chunk.iter().map(|b| format!("{:02x}", b)).collect();
        let ascii: String = chunk
            .iter()
            .map(|b| if b.is_ascii_graphic() { *b as char } else { '.' })
            .collect();
        println!("│ {:04x}  {:<48}  {}", i * 16, hex.join(" "), ascii);
    }
    println!("└─");
}

fn section(title: &str) {
    println!("\n══════════════════════════════════════════════════════");
    println!("  {}", title);
    println!("══════════════════════════════════════════════════════");
}

fn ok(msg: &str) {
    println!("  ✅ {}", msg);
}

fn fail(msg: &str) {
    println!("  ❌ {}", msg);
    std::process::exit(1);
}

// ── Main ──────────────────────────────────────────────────────────────────────

fn main() {
    println!("LIMA crypto-test v1.0");
    println!("Full node → gateway round-trip verification\n");

    // ── Step 1: Key generation ────────────────────────────────────────────────
    section("STEP 1: Key Generation");

    let node_signing_key = SigningKey::random(&mut OsRng);
    let node_verifying_key = VerifyingKey::from(&node_signing_key);
    let node_ecdh_secret = EphemeralSecret::random(&mut OsRng);
    let node_ecdh_pubkey = PublicKey::from(&node_ecdh_secret);

    let gw_signing_key = SigningKey::random(&mut OsRng);
    let gw_verifying_key = VerifyingKey::from(&gw_signing_key);
    let gw_ecdh_secret = EphemeralSecret::random(&mut OsRng);
    let gw_ecdh_pubkey = PublicKey::from(&gw_ecdh_secret);

    hex_dump("node ECDSA verifying key", &node_verifying_key.to_sec1_bytes());
    hex_dump("gateway ECDSA verifying key", &gw_verifying_key.to_sec1_bytes());
    ok("node keypair generated (simulates nRF52840 first-boot)");
    ok("gateway keypair generated");

    // ── Step 2: ECDH + HKDF ──────────────────────────────────────────────────
    section("STEP 2: ECDH → HKDF → AES-256-GCM key");

    let node_shared = node_ecdh_secret.diffie_hellman(&gw_ecdh_pubkey);
    let gw_shared   = gw_ecdh_secret.diffie_hellman(&node_ecdh_pubkey);

    // Both sides must derive identical shared secrets
    let node_shared_bytes = node_shared.raw_secret_bytes();
    let gw_shared_bytes   = gw_shared.raw_secret_bytes();

    if node_shared_bytes != gw_shared_bytes {
        fail("ECDH shared secrets do not match — key agreement failed");
    }
    ok("ECDH shared secrets match on both sides");
    hex_dump("shared secret", node_shared_bytes.as_slice());

    // HKDF-SHA256 → 32-byte AES key
    let hkdf = Hkdf::<Sha256>::new(None, node_shared_bytes.as_slice());
    let mut aes_key = [0u8; 32];
    hkdf.expand(HKDF_INFO, &mut aes_key)
        .expect("HKDF expand failed");

    hex_dump("HKDF_INFO", HKDF_INFO);
    hex_dump("derived AES-256-GCM key", &aes_key);
    ok("HKDF-SHA256 key derivation complete");

    // ── Step 3: Build payload ─────────────────────────────────────────────────
    section("STEP 3: Build LimaPayload");

    let payload = LimaPayload {
        node_id:      [0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01],
        event_type:   LimaEventType::DualBreach as u8,
        reserved:     0x00,
        sequence:     42,
        timestamp_ms: 12345678,
        accel_g:      1.23,
        delta_pa:     -15.5,
    };

    let payload_bytes = payload.to_bytes();
    hex_dump("LimaPayload (plaintext)", &payload_bytes);
    println!("  node_id:      {}", payload.node_id_str());
    println!("  event_type:   0x{:02X} ({:?})", payload.event_type,
             LimaEventType::from(payload.event_type));
    println!("  sequence:     {}", payload.sequence);
    println!("  timestamp_ms: {}", payload.timestamp_ms);
    println!("  accel_g:      {:.3}", payload.accel_g);
    println!("  delta_pa:     {:.3}", payload.delta_pa);
    ok("LimaPayload built");

    // ── Step 4: Inner ECDSA sign ──────────────────────────────────────────────
    section("STEP 4: Inner ECDSA-P256 Sign (payload → inner_sig)");

    let inner_sig: Signature = node_signing_key.sign(&payload_bytes);
    let inner_sig_bytes = inner_sig.to_bytes();
    hex_dump("inner ECDSA signature", inner_sig_bytes.as_slice());
    ok("payload signed with node ECDSA key (inner sig)");

    // Build plaintext: payload || inner_sig
    let mut plaintext = [0u8; PAYLOAD_LEN + INNER_SIG_LEN];
    plaintext[..PAYLOAD_LEN].copy_from_slice(&payload_bytes);
    plaintext[PAYLOAD_LEN..].copy_from_slice(inner_sig_bytes.as_slice());
    hex_dump("plaintext (payload || inner_sig)", &plaintext);

    // ── Step 5: AES-256-GCM encrypt ──────────────────────────────────────────
    section("STEP 5: AES-256-GCM Encrypt");

    let cipher = Aes256Gcm::new_from_slice(&aes_key)
        .expect("AES key init failed");

    // Random 12-byte nonce
    use aes_gcm::aead::rand_core::RngCore;
    let mut nonce_bytes = [0u8; NONCE_LEN];
    AesOsRng.fill_bytes(&mut nonce_bytes);
    let nonce = Nonce::from_slice(&nonce_bytes);

    let ciphertext_with_tag = cipher
        .encrypt(nonce, plaintext.as_slice())
        .expect("AES-256-GCM encryption failed");

    // aes-gcm appends tag to ciphertext — split them
    let ciphertext = &ciphertext_with_tag[..ciphertext_with_tag.len() - TAG_LEN];
    let tag        = &ciphertext_with_tag[ciphertext_with_tag.len() - TAG_LEN..];

    hex_dump("nonce (12B)", &nonce_bytes);
    hex_dump("ciphertext (88B)", ciphertext);
    hex_dump("GCM tag (16B)", tag);
    ok("AES-256-GCM encryption complete");

    // ── Step 6: Outer ECDSA sign ──────────────────────────────────────────────
    section("STEP 6: Outer ECDSA-P256 Sign (nonce || ciphertext || tag)");

    // Outer sig covers everything the gateway receives before decrypting
    let mut to_sign_outer = Vec::new();
    to_sign_outer.extend_from_slice(&nonce_bytes);
    to_sign_outer.extend_from_slice(&ciphertext_with_tag);

    let outer_sig: Signature = node_signing_key.sign(&to_sign_outer);
    let outer_sig_bytes = outer_sig.to_bytes();
    hex_dump("outer ECDSA signature", outer_sig_bytes.as_slice());
    ok("outer sig covers nonce || ciphertext || tag");

    // ── Wire boundary ─────────────────────────────────────────────────────────
    println!("\n  ╔══════════════════════════════════════════════╗");
    println!("  ║           ── WIRE BOUNDARY ──                ║");
    println!("  ║  nonce(12) ciphertext(88) tag(16) sig(64)   ║");
    println!("  ║  total: {} bytes on BLE advertisement       ║",
             NONCE_LEN + ciphertext_with_tag.len() + outer_sig_bytes.len());
    println!("  ╚══════════════════════════════════════════════╝");

    // ── Step 7: Gateway — verify outer sig ───────────────────────────────────
    section("STEP 7: Gateway — Verify Outer Signature");

    match node_verifying_key.verify(&to_sign_outer, &outer_sig) {
        Ok(_)  => ok("outer signature VALID — packet is authentic"),
        Err(e) => fail(&format!("outer signature INVALID: {}", e)),
    }

    // ── Step 8: Gateway — AES-256-GCM decrypt ────────────────────────────────
    section("STEP 8: Gateway — AES-256-GCM Decrypt");

    // Gateway derives same key via ECDH + HKDF
    let mut gw_aes_key = [0u8; 32];
    let gw_hkdf = Hkdf::<Sha256>::new(None, gw_shared_bytes.as_slice());
    gw_hkdf.expand(HKDF_INFO, &mut gw_aes_key)
        .expect("gateway HKDF expand failed");

    if gw_aes_key != aes_key {
        fail("gateway AES key does not match node AES key — HKDF mismatch");
    }
    ok("gateway derived identical AES key via ECDH + HKDF");

    let gw_cipher = Aes256Gcm::new_from_slice(&gw_aes_key)
        .expect("gateway AES key init failed");

    let decrypted = gw_cipher
        .decrypt(nonce, ciphertext_with_tag.as_slice())
        .expect("AES-256-GCM decryption failed");

    hex_dump("decrypted plaintext", &decrypted);
    ok("AES-256-GCM decryption complete");

    // ── Step 9: Gateway — verify inner sig ───────────────────────────────────
    section("STEP 9: Gateway — Verify Inner Signature");

    let dec_payload_bytes: &[u8; 24] = decrypted[..PAYLOAD_LEN]
        .try_into()
        .expect("decrypted payload wrong size");
    let dec_sig_bytes = &decrypted[PAYLOAD_LEN..PAYLOAD_LEN + INNER_SIG_LEN];

    let dec_inner_sig = Signature::from_slice(dec_sig_bytes)
        .expect("inner sig parse failed");

    match node_verifying_key.verify(dec_payload_bytes, &dec_inner_sig) {
        Ok(_)  => ok("inner signature VALID — payload is authentic"),
        Err(e) => fail(&format!("inner signature INVALID: {}", e)),
    }

    // ── Step 10: Assert round-trip integrity ──────────────────────────────────
    section("STEP 10: Assert Round-Trip Integrity");

    let recovered = LimaPayload::from_bytes(dec_payload_bytes);

    assert_eq!(payload_bytes, *dec_payload_bytes, "payload bytes mismatch");
    assert_eq!(payload.sequence,     recovered.sequence,     "sequence mismatch");
    assert_eq!(payload.timestamp_ms, recovered.timestamp_ms, "timestamp mismatch");
    assert_eq!(payload.event_type,   recovered.event_type,   "event_type mismatch");
    assert_eq!(payload.node_id,      recovered.node_id,      "node_id mismatch");

    println!("  node_id:      {}", recovered.node_id_str());
    println!("  event_type:   0x{:02X} ({:?})", recovered.event_type,
             LimaEventType::from(recovered.event_type));
    println!("  sequence:     {}", recovered.sequence);
    println!("  timestamp_ms: {}", recovered.timestamp_ms);
    println!("  accel_g:      {:.3}", recovered.accel_g);
    println!("  delta_pa:     {:.3}", recovered.delta_pa);

    ok("all fields match original payload");

    // ── Summary ───────────────────────────────────────────────────────────────
    println!("\n══════════════════════════════════════════════════════");
    println!("  LIMA CRYPTO ROUND-TRIP: ALL CHECKS PASSED");
    println!("  P-256 ECDH + HKDF-SHA256 + AES-256-GCM + ECDSA-P256");
    println!("  Encrypt-then-Sign  |  Double sig  |  184B wire format");
    println!("══════════════════════════════════════════════════════\n");
}