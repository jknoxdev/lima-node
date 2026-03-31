
### the lima frame:
```c
/* ── LF layout constants ─────────────────────────────────────────────────── */
#define LIMA_LF_HEADER_LEN      4    /* bytes covered by AAD (header only)    */
#define LIMA_LF_NONCE_LEN       12   /* AES-256-GCM IV                        */
#define LIMA_LF_PLAINTEXT_LEN   88   /* LER (24B) + inner_sig (64B)           */
#define LIMA_LF_CIPHERTEXT_LEN  88   /* GCM output same length as plaintext   */
#define LIMA_LF_TAG_LEN         16   /* AES-256-GCM auth tag                  */
#define LIMA_LF_OUTER_SIG_LEN   64   /* ECDSA-P256 outer sig                  */
#define LIMA_LF_SIGNED_LEN      120  /* bytes covered by outer_sig: LF[0..120]*/
```

### L.I.M.A. Event Record: 

```c
/* ── LIMA Event Record (LER) ─────────────────────────────────────────────── */
/*
 * Inner plaintext struct — 24 bytes.
 * Signed by node ECDSA-P256 key. Never transmitted in plaintext.
 * Always encrypted inside lima_lf_t before BLE transmission.
 *
 * Offset  Size  Field           Notes
 *      0     6  node_id         BLE MAC, big-endian
 *      6     1  event_type      lima_event_type_t
 *      7     1  reserved        Always 0x00 — alignment pad
 *      8     4  sequence        u32 LE — monotonic anti-replay counter
 *     12     4  timestamp_ms    u32 LE — RTC wall-clock epoch ms
 *     16     4  accel_g         f32 LE — IMU vector magnitude (g)
 *     20     4  delta_pa        f32 LE — barometric delta (Pa)
 *            24  TOTAL
 */
typedef struct __attribute__((packed)) {
    uint8_t  node_id[6];
    uint8_t  event_type;
    uint8_t  reserved;
    uint32_t sequence;
    uint32_t timestamp_ms;
    float    accel_g;
    float    delta_pa;
} lima_ler_t;                 /* 24 bytes */
BUILD_ASSERT(sizeof(lima_ler_t) == 24, "lima_ler_t size mismatch");
```

### L.I.M.A. Frame: 

```c
/* ── LIMA Frame (LF) ─────────────────────────────────────────────────────── */
/*
 * Outer wire envelope — 184 bytes.
 * Encrypt-then-Sign: AES-256-GCM over (LER || inner_sig),
 * then ECDSA-P256 outer sig over LF[0..120].
 *
 * Offset  Size  Field           Notes
 *      0     1  proto_version   0x02
 *      1     1  event_type      mirrors LER.event_type — gateway pre-filter
 *      2     2  reserved        0x0000
 *      4    12  nonce           AES-256-GCM IV — random per frame
 *     16    88  ciphertext      AES-256-GCM encrypt(LER 24B || inner_sig 64B)
 *    104    16  gcm_tag         AES-256-GCM authentication tag
 *    120    64  outer_sig       ECDSA-P256 sig over LF[0..120]
 *           184 TOTAL
 */
typedef struct __attribute__((packed)) {
    uint8_t  proto_version;
    uint8_t  event_type;
    uint8_t  reserved[2];
    uint8_t  nonce[12];
    uint8_t  ciphertext[88];
    uint8_t  gcm_tag[16];
    uint8_t  outer_sig[64];
} lima_lf_t;                   /* 184 bytes */
BUILD_ASSERT(sizeof(lima_lf_t) == 184, "lima_lf_t size mismatch");
```

### frame constants: 
```c
/* ── LF layout constants ─────────────────────────────────────────────────── */
#define LIMA_LF_HEADER_LEN      4    /* bytes covered by AAD (header only)    */
#define LIMA_LF_NONCE_LEN       12   /* AES-256-GCM IV                        */
#define LIMA_LF_PLAINTEXT_LEN   88   /* LER (24B) + inner_sig (64B)           */
#define LIMA_LF_CIPHERTEXT_LEN  88   /* GCM output same length as plaintext   */
#define LIMA_LF_TAG_LEN         16   /* AES-256-GCM auth tag                  */
#define LIMA_LF_OUTER_SIG_LEN   64   /* ECDSA-P256 outer sig                  */
#define LIMA_LF_SIGNED_LEN      120  /* bytes covered by outer_sig: LF[0..120]*/
```

## decryption path

```bash
DB raw_blob (182B, btleplug-stripped)
  → reconstruct full 184B LF
  → AAD         = LF[0..4]      # (header)
  → nonce       = LF[4..16]     # (12B)
  → ciphertext  = LF[16..104]   # (88B)
  → tag         = LF[104..120]  # (16B)
  → AES-256-GCM = decrypt       # → 88B plaintext
  → first 24B                   # LER
  → remaining 64B               # inner_sig (can ignore for now)
  ```

### scaffold
```bash
client/
├── Cargo.toml
└── src/
    ├── main.rs     # CLI entrypoint, args
    ├── db.rs       # RUD ops against lima_gateway.db
    ├── crypto.rs   # AES-256-GCM decrypt, LER parse
    └── display.rs  # formatted LER output
```