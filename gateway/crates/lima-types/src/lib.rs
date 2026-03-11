//! LIMA shared types and wire format constants
//!
//! This crate is the single source of truth for:
//!   - LimaPayload struct (mirrors lima_payload_t in firmware/src/crypto.h)
//!   - Wire format constants and offsets
//!   - HKDF_INFO constant (must match firmware crypto.c exactly)
//!
//! Any change here requires a matching change in firmware/src/crypto.h

use serde::{Deserialize, Serialize};

// ── Wire format constants ─────────────────────────────────────────────────────

pub const PAYLOAD_LEN: usize = 24;       // lima_payload_t on wire
pub const INNER_SIG_LEN: usize = 64;     // ECDSA-P256 sig over plaintext
pub const PLAINTEXT_LEN: usize = PAYLOAD_LEN + INNER_SIG_LEN; // 88B

pub const NONCE_LEN: usize = 12;         // AES-256-GCM nonce
pub const TAG_LEN: usize = 16;           // AES-256-GCM auth tag
pub const OUTER_SIG_LEN: usize = 64;     // ECDSA-P256 sig over ciphertext
pub const HEADER_LEN: usize = 4;         // packet header

pub const CIPHERTEXT_LEN: usize = PLAINTEXT_LEN + TAG_LEN; // 104B
pub const PACKET_LEN: usize =
    HEADER_LEN + NONCE_LEN + CIPHERTEXT_LEN + OUTER_SIG_LEN; // 184B

// ── HKDF info string ──────────────────────────────────────────────────────────
//
// CRITICAL: This must be byte-identical to HKDF_INFO in firmware/src/crypto.c
// Do not modify without updating both sides simultaneously.

pub const HKDF_INFO: &[u8] = &[
    0x2e, 0x29, 0xee, 0x16, 0xe8, 0x10, 0x6d, 0x8a,
    0xdd, 0xbb, 0x50, 0xe2, 0x12, 0x16, 0x3d, 0xfd,
    0xa8, 0xf4, 0x24, 0xe2, 0xc9, 0x7d, 0x4b, 0xd3,
    0x17, 0xb9, 0x9a, 0x96, 0xe0, 0x7e, 0x5c, 0x6f,
];

// ── LimaPayload ───────────────────────────────────────────────────────────────
//
// Mirrors lima_payload_t in firmware/src/crypto.h
// Layout is fixed — any change breaks the wire format.
//
// Offset  Size  Field
// 0       6     node_id      (BLE MAC, big-endian)
// 6       1     event_type   (LimaEventType)
// 7       1     reserved     (zero)
// 8       4     sequence     (u32 little-endian, monotonic)
// 12      4     timestamp_ms (u32 little-endian, k_uptime_get_32)
// 16      4     accel_g      (f32 little-endian, IMU magnitude)
// 20      4     delta_pa     (f32 little-endian, baro delta)
//                            = 24 bytes total

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LimaPayload {
    pub node_id:      [u8; 6],
    pub event_type:   u8,
    pub reserved:     u8,
    pub sequence:     u32,
    pub timestamp_ms: u32,
    pub accel_g:      f32,
    pub delta_pa:     f32,
}

impl LimaPayload {
    /// Serialize to wire format (24 bytes, little-endian)
    pub fn to_bytes(&self) -> [u8; PAYLOAD_LEN] {
        let mut buf = [0u8; PAYLOAD_LEN];
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
    pub fn from_bytes(data: &[u8; PAYLOAD_LEN]) -> Self {
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

// ── LimaEventType ─────────────────────────────────────────────────────────────

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LimaEventType {
    MotionOnly  = 0x01,
    BaroOnly    = 0x02,
    DualBreach  = 0x03,
    Heartbeat   = 0x04,
    Unknown     = 0xFF,
}

impl From<u8> for LimaEventType {
    fn from(v: u8) -> Self {
        match v {
            0x01 => Self::MotionOnly,
            0x02 => Self::BaroOnly,
            0x03 => Self::DualBreach,
            0x04 => Self::Heartbeat,
            _    => Self::Unknown,
        }
    }
}