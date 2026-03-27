//! LIMA shared types and wire format constants
//!
//! This crate is the single source of truth for the Rust side of:
//!   - LimaEventRecord (LER) — mirrors lima_ler_t in firmware/src/crypto.h
//!   - LimaFrame (LF)        — mirrors lima_lf_t in firmware/src/ble.h
//!   - Wire format constants and offsets
//!   - HKDF_INFO constant (must match firmware crypto.c exactly)
//!
//! Spec: docs/dev/frame-record-spec.md
//! Any change here requires a matching change in firmware/src/crypto.h or ble.h

use serde::{Deserialize, Serialize};

// ── Wire format constants ─────────────────────────────────────────────────────

pub const LER_LEN: usize       = 24;   // lima_ler_t on wire
pub const INNER_SIG_LEN: usize = 64;   // ECDSA-P256 inner sig over LER
pub const PLAINTEXT_LEN: usize = LER_LEN + INNER_SIG_LEN; // 88B

pub const HEADER_LEN: usize    = 4;    // proto_version + event_type + reserved[2]
pub const NONCE_LEN: usize     = 12;   // AES-256-GCM IV
pub const CIPHERTEXT_LEN: usize = 88;  // AES-256-GCM encrypt(LER || inner_sig)
pub const TAG_LEN: usize       = 16;   // AES-256-GCM auth tag
pub const OUTER_SIG_LEN: usize = 64;   // ECDSA-P256 outer sig over LF[0..120]

pub const LF_LEN: usize =
    HEADER_LEN + NONCE_LEN + CIPHERTEXT_LEN + TAG_LEN + OUTER_SIG_LEN; // 184B

// Offsets within LF
pub const LF_OFFSET_PROTO:      usize = 0;
pub const LF_OFFSET_EVENT_TYPE: usize = 1;
pub const LF_OFFSET_RESERVED:   usize = 2;
pub const LF_OFFSET_NONCE:      usize = 4;
pub const LF_OFFSET_CIPHERTEXT: usize = 16;
pub const LF_OFFSET_GCM_TAG:    usize = 104;
pub const LF_OFFSET_OUTER_SIG:  usize = 120;
pub const LF_SIGNED_BYTES:      usize = 120; // outer_sig covers LF[0..120]

// ── HKDF info string ──────────────────────────────────────────────────────────
//
// CRITICAL: Must be byte-identical to HKDF_INFO in firmware/src/crypto.c
// Do not modify without updating both sides simultaneously.

pub const HKDF_INFO: &[u8] = &[
    0x2e, 0x29, 0xee, 0x16, 0xe8, 0x10, 0x6d, 0x8a,
    0xdd, 0xbb, 0x50, 0xe2, 0x12, 0x16, 0x3d, 0xfd,
    0xa8, 0xf4, 0x24, 0xe2, 0xc9, 0x7d, 0x4b, 0xd3,
    0x17, 0xb9, 0x9a, 0x96, 0xe0, 0x7e, 0x5c, 0x6f,
];

// ── LimaEventRecord (LER) ─────────────────────────────────────────────────────
//
// Mirrors lima_ler_t in firmware/src/crypto.h
// Layout is fixed — any change breaks the wire format.
//
// Offset  Size  Field           Notes
// 0       6     node_id         BLE MAC, big-endian
// 6       1     event_type      LimaEventType
// 7       1     reserved        Always 0x00
// 8       4     sequence        u32 LE — monotonic anti-replay
// 12      4     timestamp_ms    u32 LE — RTC wall-clock epoch ms
// 16      4     accel_g         f32 LE — IMU magnitude (g)
// 20      4     delta_pa        f32 LE — baro delta (Pa)
//               = 24 bytes total

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LimaEventRecord {
    pub node_id:      [u8; 6],
    pub event_type:   u8,
    pub reserved:     u8,
    pub sequence:     u32,
    pub timestamp_ms: u32,
    pub accel_g:      f32,
    pub delta_pa:     f32,
}

impl LimaEventRecord {
    /// Serialize to wire format (24 bytes, little-endian)
    pub fn to_bytes(&self) -> [u8; LER_LEN] {
        let mut buf = [0u8; LER_LEN];
        buf[0..6].copy_from_slice(&self.node_id);
        buf[6] = self.event_type;
        buf[7] = self.reserved;
        buf[8..12].copy_from_slice(&self.sequence.to_le_bytes());
        buf[12..16].copy_from_slice(&self.timestamp_ms.to_le_bytes());
        buf[16..20].copy_from_slice(&self.accel_g.to_le_bytes());
        buf[20..24].copy_from_slice(&self.delta_pa.to_le_bytes());
        buf
    }

    /// Deserialize from wire format
    pub fn from_bytes(data: &[u8; LER_LEN]) -> Self {
        let mut node_id = [0u8; 6];
        node_id.copy_from_slice(&data[0..6]);
        Self {
            node_id,
            event_type:   data[6],
            reserved:     data[7],
            sequence:     u32::from_le_bytes(data[8..12].try_into().unwrap()),
            timestamp_ms: u32::from_le_bytes(data[12..16].try_into().unwrap()),
            accel_g:      f32::from_le_bytes(data[16..20].try_into().unwrap()),
            delta_pa:     f32::from_le_bytes(data[20..24].try_into().unwrap()),
        }
    }

    /// Format node_id as MAC string
    pub fn node_id_str(&self) -> String {
        format!(
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            self.node_id[0], self.node_id[1], self.node_id[2],
            self.node_id[3], self.node_id[4], self.node_id[5]
        )
    }
}

// ── LimaFrame (LF) ───────────────────────────────────────────────────────────
//
// Mirrors lima_lf_t in firmware/src/ble.h
// Outer wire envelope — 184 bytes.
// Transmitted as BLE 5.0 extended advertising manufacturer-specific AD data.

#[derive(Debug, Clone)]
pub struct LimaFrame {
    pub proto_version: u8,
    pub event_type:    u8,
    pub reserved:      [u8; 2],
    pub nonce:         [u8; NONCE_LEN],
    pub ciphertext:    [u8; CIPHERTEXT_LEN],
    pub gcm_tag:       [u8; TAG_LEN],
    pub outer_sig:     [u8; OUTER_SIG_LEN],
}

impl LimaFrame {
    /// Deserialize from raw 184-byte BLE AD data
    pub fn from_bytes(data: &[u8; LF_LEN]) -> Self {
        let mut nonce      = [0u8; NONCE_LEN];
        let mut ciphertext = [0u8; CIPHERTEXT_LEN];
        let mut gcm_tag    = [0u8; TAG_LEN];
        let mut outer_sig  = [0u8; OUTER_SIG_LEN];

        nonce.copy_from_slice(&data[LF_OFFSET_NONCE..LF_OFFSET_NONCE + NONCE_LEN]);
        ciphertext.copy_from_slice(&data[LF_OFFSET_CIPHERTEXT..LF_OFFSET_CIPHERTEXT + CIPHERTEXT_LEN]);
        gcm_tag.copy_from_slice(&data[LF_OFFSET_GCM_TAG..LF_OFFSET_GCM_TAG + TAG_LEN]);
        outer_sig.copy_from_slice(&data[LF_OFFSET_OUTER_SIG..LF_OFFSET_OUTER_SIG + OUTER_SIG_LEN]);

        Self {
            proto_version: data[LF_OFFSET_PROTO],
            event_type:    data[LF_OFFSET_EVENT_TYPE],
            reserved:      [data[2], data[3]],
            nonce,
            ciphertext,
            gcm_tag,
            outer_sig,
        }
    }

    /// Serialize to wire format (184 bytes)
    pub fn to_bytes(&self) -> [u8; LF_LEN] {
        let mut buf = [0u8; LF_LEN];
        buf[LF_OFFSET_PROTO]      = self.proto_version;
        buf[LF_OFFSET_EVENT_TYPE] = self.event_type;
        buf[2..4].copy_from_slice(&self.reserved);
        buf[LF_OFFSET_NONCE..LF_OFFSET_NONCE + NONCE_LEN]
            .copy_from_slice(&self.nonce);
        buf[LF_OFFSET_CIPHERTEXT..LF_OFFSET_CIPHERTEXT + CIPHERTEXT_LEN]
            .copy_from_slice(&self.ciphertext);
        buf[LF_OFFSET_GCM_TAG..LF_OFFSET_GCM_TAG + TAG_LEN]
            .copy_from_slice(&self.gcm_tag);
        buf[LF_OFFSET_OUTER_SIG..LF_OFFSET_OUTER_SIG + OUTER_SIG_LEN]
            .copy_from_slice(&self.outer_sig);
        buf
    }

    /// Bytes covered by outer_sig — LF[0..120]
    pub fn signed_bytes(&self) -> [u8; LF_SIGNED_BYTES] {
        let full = self.to_bytes();
        let mut out = [0u8; LF_SIGNED_BYTES];
        out.copy_from_slice(&full[..LF_SIGNED_BYTES]);
        out
    }
}

// ── LimaEventType ─────────────────────────────────────────────────────────────
//
// Must match lima_event_type_t in firmware/src/events.h

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LimaEventType {
    PressureBreach = 0x01,  // LIMA_EVT_PRESSURE_BREACH
    MotionDetected = 0x02,  // LIMA_EVT_MOTION_DETECTED
    DualBreach     = 0x03,  // LIMA_EVT_DUAL_BREACH
    Heartbeat      = 0x04,  // LIMA_EVT_WAKEUP
    Unknown        = 0xFF,
}

impl From<u8> for LimaEventType {
    fn from(v: u8) -> Self {
        match v {
            0x01 => Self::PressureBreach,
            0x02 => Self::MotionDetected,
            0x03 => Self::DualBreach,
            0x04 => Self::Heartbeat,
            _    => Self::Unknown,
        }
    }
}
