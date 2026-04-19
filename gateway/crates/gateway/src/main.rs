//! LIMA Gateway — raw HCI scanner, signature verification, SQLite audit log, ratatui TUI
//!
//! Pipeline:
//!   raw HCI socket (AF_BLUETOOTH/BTPROTO_HCI/HCI_CHANNEL_RAW on hci1)
//!   → LE Extended Advertising Report (subevent 0x0D)
//!   → extract manufacturer payload
//!   → verify outer ECDSA sig
//!   → store raw encrypted blob in SQLite
//!   → publish to MQTT
//!   → update ratatui TUI
//!
//! Scan layer bypasses bluetoothd D-Bus coalescing entirely.
//! Reads all extended advertising events directly from the kernel HCI layer.
//! bluetoothd must have hci1 up and scanning active (or task sends its own scan cmds).

use std::{
    io,
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use crossterm::{
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use p256::ecdsa::{signature::Verifier, Signature, VerifyingKey};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout},
    style::{Color, Modifier, Style},
    widgets::{Block, Borders, Cell, Row, Table, TableState},
    Terminal,
};
use rusqlite::{params, Connection};
use tokio::sync::Mutex;
use tokio::signal::unix::{signal, SignalKind};
use lima_types::{LF_LEN, LF_SIGNED_BYTES, LF_OFFSET_OUTER_SIG, OUTER_SIG_LEN};
use rumqttc::{AsyncClient, MqttOptions, QoS};
use dirs;

// ── HCI constants ─────────────────────────────────────────────────────────────

const BTPROTO_HCI:          i32 = 1;
const HCI_CHANNEL_RAW:      u16 = 0;
const HCI_CHANNEL_USER:     u16 = 1;
const SOL_HCI:              i32 = 0;
const HCI_FILTER_SOCKOPT:   i32 = 2;
const HCI_EVENT_PKT:        u32 = 4;   // packet type byte
const HCI_EVENT_CODE_LE_META: u8 = 0x3E;
const LE_EXT_ADV_REPORT_SUBEVENT: u8 = 0x0D;

// HCI command opcodes (OGF=0x08 LE Controller)
const HCI_LE_SET_EXT_SCAN_PARAMS:  u16 = 0x2041;
const HCI_LE_SET_EXT_SCAN_ENABLE:  u16 = 0x2042;
const HCI_COMMAND_PKT:             u8  = 0x01;

/// HCI socket address (matches struct sockaddr_hci in kernel bluetooth/hci.h)
#[repr(C)]
struct SockAddrHci {
    hci_family:  u16,   // AF_BLUETOOTH = 31
    hci_dev:     u16,   // adapter index (hci0=0, hci1=1, ...)
    hci_channel: u16,   // HCI_CHANNEL_RAW = 0
}

/// HCI socket filter (matches struct hci_filter in bluetooth/hci.h, packed)
#[repr(C, packed)]
struct HciFilter {
    type_mask:  u32,     // bit N = accept packet type N
    event_mask: [u32; 2],// bit N = accept event code N (64 bits total)
    opcode:     u16,     // filter by opcode (0 = all)
}

// ── Constants ─────────────────────────────────────────────────────────────────

// Current node public key — clock provisioned 2026-04-19
const TEST_NODE_PUBKEY_HEX: &str = concat!(
    "04 8d a8 7d 0a 4d df c4  16 c4 01 82 6e d8 ea 0d ",
    "b2 9e c3 65 13 50 69 69  b8 8c 83 79 de 06 e3 10 ",
    "3e 42 a3 9e 66 e8 f3 e7  aa 62 d2 aa 24 18 4d 88 ",
    "e1 1f 2c 7a aa 9d e8 a0  48 84 90 5b 59 ed 48 7f ",
    "d7"
);

const DB_PATH:  &str = "lima_gateway.db";
const NODE_MAC: &str = "dev_E3_79_63_12_EF_B1";

// ── MQTT constants ────────────────────────────────────────────────────────────

const MQTT_HOST:      &str = "localhost";
const MQTT_PORT:      u16  = 1883;
const MQTT_CLIENT_ID: &str = "lima-gateway";
const NTFY_DEBOUNCE_SECS: u64 = 5;

fn load_ntfy_topic() -> String {
    let path = dirs::home_dir()
        .expect("no home dir")
        .join(".lima/keys/ntfy_topic");
    std::fs::read_to_string(&path)
        .expect("failed to read ntfy_topic")
        .trim()
        .to_string()
}

// Topic schema:
//   lima/nodes/{node_id}/frames   — raw verified LF blob (hex) per frame
//   lima/gateway/health           — gateway online/offline (retained)
const MQTT_TOPIC_HEALTH: &str = "lima/gateway/health";

fn mqtt_topic_frames(node_id: &str) -> String {
    // sanitize btleplug node_id ("dev_E3_79_63_12_EF_B1") for MQTT topic
    let clean = node_id.replace('/', "-").replace('_', "-");
    format!("lima/nodes/{}/frames", clean)
}

// ── MQTT frame to publish ─────────────────────────────────────────────────────

struct MqttFrame {
    topic:   String,
    payload: String,
    retain:  bool,
}

// ── Event record ──────────────────────────────────────────────────────────────

#[derive(Clone)]
struct EventRecord {
    id:           i64,
    node_id:      String,
    received_at:  u64,
    sig_verified: bool,
    rssi:         i8,
    frame_type:   u8,  
    raw_blob_hex: String,
}

// ── App state ─────────────────────────────────────────────────────────────────

struct App {
    events:        Vec<EventRecord>,
    table_state:   TableState,
    total_rx:      u64,
    total_valid:   u64,
    total_invalid: u64,
    last_event_at: Option<u64>,
}

impl App {
    fn new() -> Self {
        Self {
            events:        Vec::new(),
            table_state:   TableState::default(),
            total_rx:      0,
            total_valid:   0,
            total_invalid: 0,
            last_event_at: None,
        }
    }

    fn push(&mut self, rec: EventRecord) {
        self.total_rx += 1;
        self.last_event_at = Some(rec.received_at);
        if rec.sig_verified {
            self.total_valid += 1;
        } else {
            self.total_invalid += 1;
        }
        self.events.insert(0, rec); // newest first
        if self.events.len() > 100 {
            self.events.truncate(100);
        }
    }
}

// ── Database ──────────────────────────────────────────────────────────────────

fn db_init(conn: &Connection) -> rusqlite::Result<()> {
    conn.execute_batch("PRAGMA journal_mode=WAL;")?;
    conn.execute_batch("PRAGMA busy_timeout=1000;")?;
    conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS events (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id       TEXT    NOT NULL,
            received_at   INTEGER NOT NULL,
            sig_verified  INTEGER NOT NULL,
            rssi          INTEGER NOT NULL,
            frame_type    INTEGER NOT NULL DEFAULT 0,
            raw_blob      BLOB    NOT NULL
        );",
    )
}

fn db_insert(conn: &Connection, rec: &EventRecord) -> rusqlite::Result<i64> {
    conn.execute(
        "INSERT INTO events
            (node_id, received_at, sig_verified, rssi, frame_type, raw_blob)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            rec.node_id,
            rec.received_at,
            rec.sig_verified as i32,
            rec.rssi,
            rec.frame_type as i32, 
            rec.raw_blob_hex,
        ],
    )?;
    Ok(conn.last_insert_rowid())
}

// ── Crypto ────────────────────────────────────────────────────────────────────

fn load_test_verifying_key() -> VerifyingKey {
    let bytes = hex::decode(TEST_NODE_PUBKEY_HEX.replace(['\n', ' '], ""))
        .expect("TEST_NODE_PUBKEY_HEX invalid hex");
    VerifyingKey::from_sec1_bytes(&bytes)
        .expect("TEST_NODE_PUBKEY_HEX invalid P-256 key")
}

/// Verify ECDSA-P256 outer signature over a LIMA Frame (LF).
///
/// BLE manufacturer specific data carries the 184B LF split as:
///   mfr_id (2B LE) = LF[0] (proto_version) | LF[1] (event_type) << 8
///   payload (182B) = LF[2..184]
///
/// Reconstruct full 184B LF then verify outer_sig over LF[0..120].
///
/// LF layout (184B):
///   [0]       proto_version
///   [1]       event_type
///   [2-3]     reserved
///   [4-15]    nonce (12B)
///   [16-103]  ciphertext (88B)
///   [104-119] gcm_tag (16B)
///   [120-183] outer_sig (64B) — NOT included in signed region

fn verify_outer_sig(mfr_id: u16, payload: &[u8], vk: &VerifyingKey) -> bool {
    const STRIPPED_LEN: usize = LF_LEN - 2; // 182

    if payload.len() != STRIPPED_LEN {
        return false;
    }

    // Reconstruct full 184B LF — mfr_id is little-endian in BLE
    let mut full_lf = [0u8; LF_LEN];
    full_lf[0] = (mfr_id & 0xFF) as u8;         // proto_version
    full_lf[1] = ((mfr_id >> 8) & 0xFF) as u8;  // event_type
    full_lf[2..].copy_from_slice(payload);

    let signed    = &full_lf[..LF_SIGNED_BYTES];
    let sig_bytes = &full_lf[LF_OFFSET_OUTER_SIG..LF_OFFSET_OUTER_SIG + OUTER_SIG_LEN];

    match Signature::from_slice(sig_bytes) {
        Ok(sig) => vk.verify(signed, &sig).is_ok(),
        Err(_)  => false,
    }
}

// ── TUI rendering ─────────────────────────────────────────────────────────────

fn ui(f: &mut ratatui::Frame, app: &mut App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),  // header
            Constraint::Min(0),     // table
            Constraint::Length(3),  // footer
        ])
        .split(f.size());

    // ── Header ────────────────────────────────────────────────────────────────
    let header_text = format!(
        " LIMA Gateway  |  rx: {}  valid: {}  invalid: {}",
        app.total_rx, app.total_valid, app.total_invalid
    );
    let header = Block::default()
        .borders(Borders::ALL)
        .title(header_text)
        .style(Style::default().fg(Color::Cyan));
    f.render_widget(header, chunks[0]);

    // ── Event table ───────────────────────────────────────────────────────────
    let header_cells = ["time", "node_id", "evt", "seq", "sig", "rssi"]
        .iter()
        .map(|h| Cell::from(*h).style(Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)));
    let table_header = Row::new(header_cells).height(1).bottom_margin(1);

    let rows = app.events.iter().map(|rec| {
        // raw_blob_hex is LF[2..184] (182B, mfr_id stripped):
        //   [0-1]   reserved
        //   [2-13]  nonce
        //   [14-101] ciphertext
        //   [102-117] gcm_tag
        //   [118-181] outer_sig
        
        let raw = hex::decode(&rec.raw_blob_hex).unwrap_or_default();

        let evt = raw.get(1)
            .map(|b| format!("0x{:02X}", b))
            .unwrap_or_else(|| "?".to_string());

        // seq is inside ciphertext — not visible without decryption
        let seq = "--".to_string();

        // outer_sig starts at offset 120 in 182B payload (LF offset 120, subtract 2 stripped = 118)
        let sig_fp = if raw.len() >= 122 {
            format!("{:02X}{:02X}{:02X}{:02X}",
                raw[118], raw[119], raw[120], raw[121])
        } else {
            "?".to_string()
        };

        let sig_cell = if rec.sig_verified {
            Cell::from(format!("✅ {}", sig_fp))
                .style(Style::default().fg(Color::Green))
        } else {
            Cell::from(format!("❌ {}", sig_fp))
                .style(Style::default().fg(Color::Red).add_modifier(Modifier::BOLD))
        };

        Row::new(vec![
            Cell::from(format_timestamp(rec.received_at)),
            Cell::from(rec.node_id.clone()),
            Cell::from(evt),
            Cell::from(seq),
            sig_cell,
            Cell::from(format!("{} dBm", rec.rssi)),
        ])
        .height(1)
    });

    let table = Table::new(
        rows,
        [
            Constraint::Length(10),  // time
            Constraint::Length(20),  // node_id
            Constraint::Length(8),   // evt
            Constraint::Length(6),   // seq
            Constraint::Length(16),  // sig fingerprint
            Constraint::Length(10),  // rssi
        ],
    )
    .header(table_header)
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Events (newest first) "),
    )
    .highlight_style(Style::default().add_modifier(Modifier::REVERSED));

    // fixes missing rows from render issue
    f.render_widget(ratatui::widgets::Clear, chunks[1]);
    f.render_stateful_widget(table, chunks[1], &mut app.table_state);

    // ── Footer ────────────────────────────────────────────────────────────────
    let last = app.events.first().map(|e| {
        let raw = hex::decode(&e.raw_blob_hex).unwrap_or_default();
        let sig_fp = if raw.len() >= 122 {
            format!("{:02X}{:02X}{:02X}{:02X}",
               raw[118], raw[119], raw[120], raw[121])
        } else {
            "????".to_string()
        };
        format!(
            "{}  sig[0..3]={}",
            if e.sig_verified { "✓ VALID" } else { "✗ INVALID" },
            sig_fp
        )
    })
    .unwrap_or_else(|| "--".to_string());

    let footer_title = format!(
        " q: quit  |  DB: {}  |  last: {}  |  last_rx: {} ",
        DB_PATH,
        last,
        app.last_event_at
            .map(|t| format_timestamp(t))
            .unwrap_or_else(|| "--".to_string()),
    );

    let footer = Block::default()
        .borders(Borders::ALL)
        .title(footer_title)
        .style(Style::default().fg(Color::DarkGray));
    f.render_widget(footer, chunks[2]);
}

fn format_timestamp(ts_ms: u64) -> String {
    let secs = ts_ms / 1000;
    let h = (secs % 86400) / 3600;
    let m = (secs % 3600) / 60;
    let s = secs % 60;
    format!("{:02}:{:02}:{:02}", h, m, s)
}

async fn ntfy_notify(client: reqwest::Client, url: String) {
    if let Err(e) = client
        .post(&url)
        .header("Title", "LIMA")
        .header("Priority", "high")
        .header("Tags", "lock")
        .body("integrity event detected")
        .send()
        .await
    {
        eprintln!("ntfy publish error: {}", e);
    }
}

// ── MQTT task ─────────────────────────────────────────────────────────────────

async fn mqtt_task(
    mut rx:       tokio::sync::mpsc::Receiver<MqttFrame>,
    mqtt_options: MqttOptions,
) {
    const RETRY_DELAY:     Duration = Duration::from_secs(5);
    const PUBLISH_TIMEOUT: Duration = Duration::from_secs(5);

    loop {
        let (client, mut eventloop) = AsyncClient::new(mqtt_options.clone(), 64);
        let (dead_tx, mut dead_rx) = tokio::sync::oneshot::channel::<()>();

        tokio::spawn(async move {
            loop {
                match eventloop.poll().await {
                    Ok(_)  => {}
                    Err(e) => {
                        eprintln!("[MQTT] eventloop error: {e}");
                        let _ = dead_tx.send(());
                        return;
                    }
                }
            }
        });
        
        let _ = client.publish(MQTT_TOPIC_HEALTH, QoS::AtLeastOnce, true, "online").await;
        eprintln!("[MQTT] connected to broker {}:{}", MQTT_HOST, MQTT_PORT);

        // Drain incoming frames until connection dies
        'publish: loop {
            tokio::select! {
                _ = &mut dead_rx => {
                    eprintln!("[MQTT] connection lost — reconnecting");
                    break 'publish;
                }
                frame = rx.recv() => {
                    match frame {
                        None => return, // sender dropped
                        Some(frame) => {
                            match tokio::time::timeout(
                                PUBLISH_TIMEOUT,
                                client.publish(&frame.topic, QoS::AtLeastOnce, frame.retain, frame.payload.clone()),
                            ).await {
                                Ok(Ok(_))  => {}
                                Ok(Err(e)) => {
                                    eprintln!("[MQTT] publish error: {e} — reconnecting");
                                    break 'publish;
                                }
                                Err(_) => {
                                    eprintln!("[MQTT] publish timeout — broker unreachable");
                                    break 'publish;
                                }
                            }
                        }
                    }
                }
            }
        }

        tokio::time::sleep(RETRY_DELAY).await;
        eprintln!("[MQTT] reconnecting...");
    }
}

// ── Raw HCI helpers ───────────────────────────────────────────────────────────

/// Open AF_BLUETOOTH / BTPROTO_HCI / HCI_CHANNEL_RAW socket on `hci_dev`.
/// Returns the raw fd on success; logs and returns -1 on failure.
///
/// Requires CAP_NET_RAW (set via AmbientCapabilities in the systemd unit).
/// HCI_CHANNEL_RAW taps into the kernel HCI event stream before bluetoothd's
/// D-Bus layer, so we receive every advertising event the hardware delivers.
fn hci_open_raw(hci_dev: u16) -> libc::c_int {
    unsafe {
        // SOCK_CLOEXEC = O_CLOEXEC for sockets
        let sock = libc::socket(
            libc::AF_BLUETOOTH,
            libc::SOCK_RAW | libc::SOCK_CLOEXEC,
            BTPROTO_HCI,
        );
        if sock < 0 {
            eprintln!("[HCI] socket() failed: {}", std::io::Error::last_os_error());
            return -1;
        }

        // Bind to the target adapter
        let addr = SockAddrHci {
            hci_family:  libc::AF_BLUETOOTH as u16,
            hci_dev,
            hci_channel: HCI_CHANNEL_USER,
        };
        let ret = libc::bind(
            sock,
            &addr as *const SockAddrHci as *const libc::sockaddr,
            std::mem::size_of::<SockAddrHci>() as libc::socklen_t,
        );
        if ret < 0 {
            eprintln!("[HCI] bind(hci{}) failed: {}", hci_dev, std::io::Error::last_os_error());
            libc::close(sock);
            return -1;
        }

        // Set HCI filter: accept event packets only; all event codes.
        // We filter to subevent 0x0D in software — keeping this broad avoids
        // missing events if subevents change.
        let filter = HciFilter {
            type_mask:  1 << HCI_EVENT_PKT,    // 0x10 — event packets only
            event_mask: [0xFFFF_FFFF, 0xFFFF_FFFF], // all event codes
            opcode:     0,                      // no opcode filter
        };
        let ret = libc::setsockopt(
            sock,
            SOL_HCI,
            HCI_FILTER_SOCKOPT,
            &filter as *const HciFilter as *const libc::c_void,
            std::mem::size_of::<HciFilter>() as libc::socklen_t,
        );
        if ret < 0 {
            // Non-fatal on recent kernels — HCI_FILTER may not be supported on
            // HCI_CHANNEL_RAW. Software filtering in parse_ext_adv_report is sufficient.
            eprintln!("[HCI] setsockopt(HCI_FILTER) unsupported ({}), continuing without kernel filter",
                      std::io::Error::last_os_error());
        }
        sock 
    }
}

/// Send LE Set Extended Scan Parameters + LE Set Extended Scan Enable via a
/// raw HCI socket.  HCI_CHANNEL_RAW allows writing HCI commands as long as
/// the adapter is UP (kernel check in hci_sock_sendmsg).
///
/// Parameters: passive scan, 1M PHY only, 30ms interval/window (100% duty
/// cycle), no duplicate filtering (critical — we want every frame).
///
/// Errors are logged but not fatal; if the adapter is already scanning the
/// events will arrive anyway.
fn hci_start_ext_scan(sock: libc::c_int) {
    
    // HCI Reset (0x0C03)
    // let reset_cmd: [u8; 4] = [HCI_COMMAND_PKT, 0x03, 0x0C, 0x00];
    // unsafe { libc::write(sock, reset_cmd.as_ptr() as *const libc::c_void, 4); }
    // std::thread::sleep(Duration::from_millis(500)); // wait for controller ready
    
    // ── LE Set Extended Scan Parameters (0x2041) ──────────────────────────
    // 8 parameter bytes for 1M PHY only:
    //   own_addr_type(1) + filter_policy(1) + scanning_phys(1)
    //   + [1M: scan_type(1) + interval(2) + window(2)]
    let params_cmd: [u8; 12] = [
        HCI_COMMAND_PKT,
        (HCI_LE_SET_EXT_SCAN_PARAMS & 0xFF) as u8,
        (HCI_LE_SET_EXT_SCAN_PARAMS >> 8)   as u8,
        8,          // parameter length
        0x00,       // Own_Address_Type: public
        0x00,       // Scanning_Filter_Policy: accept all
        0x01,       // Scanning_PHYs: 1M only
        0x00,       // Scan_Type: passive
        0x30, 0x00, // Scan_Interval: 30ms
        0x30, 0x00, // Scan_Window: 30ms
    ];

    unsafe {
        let ret = libc::write(
            sock,
            params_cmd.as_ptr() as *const libc::c_void,
            params_cmd.len(),
        );
        if ret < 0 {
            eprintln!("[HCI] write(LE_SET_EXT_SCAN_PARAMS) failed: {}",
                      std::io::Error::last_os_error());
        } else {
            eprintln!("[HCI] LE Set Extended Scan Parameters sent");
        }
    }

    // Brief pause — give the controller time to apply params before enable.
    std::thread::sleep(Duration::from_millis(20));

    // ── LE Set Extended Scan Enable (0x2042) ──────────────────────────────
    // 6 parameter bytes:
    //   enable(1) + filter_duplicates(1) + duration(2) + period(2)
    let enable_cmd: [u8; 10] = [
        HCI_COMMAND_PKT,
        (HCI_LE_SET_EXT_SCAN_ENABLE & 0xFF) as u8,
        (HCI_LE_SET_EXT_SCAN_ENABLE >> 8)   as u8,
        6,          // parameter length
        0x01,       // Enable: 1
        0x00,       // Filter_Duplicates: 0 — receive every frame
        0x00, 0x00, // Duration: 0 = continuous
        0x00, 0x00, // Period: 0
    ];

    unsafe {
        let ret = libc::write(
            sock,
            enable_cmd.as_ptr() as *const libc::c_void,
            enable_cmd.len(),
        );
        if ret < 0 {
            eprintln!("[HCI] write(LE_SET_EXT_SCAN_ENABLE) failed: {}",
                      std::io::Error::last_os_error());
    } else {            
        eprintln!("[HCI] LE Set Extended Scan Enable sent — scanning hci{}", 0);
        }
    }
}

// ── HCI packet parsing ────────────────────────────────────────────────────────

/// Parse manufacturer specific AD structure from raw AD data bytes.
///
/// Returns (mfr_id, payload) where:
///   mfr_id  = company ID, u16 LE  (= LF[0] | LF[1]<<8 after btleplug stripping)
///   payload = everything after the company ID bytes (= LF[2..184], 182B for LIMA)
///
/// Only the first matching AD type 0xFF entry is returned.
fn parse_ad_manufacturer(ad_data: &[u8]) -> Option<(u16, &[u8])> {
    let mut i = 0;
    while i < ad_data.len() {
        let length = ad_data[i] as usize;
        if length == 0 {
            break; // zero-length terminates AD structures
        }
        if i + 1 + length > ad_data.len() {
            break; // malformed
        }

        let ad_type    = ad_data[i + 1];
        let ad_content = &ad_data[i + 2..i + 1 + length]; // length includes type byte

        if ad_type == 0xFF && ad_content.len() >= 2 {
            let mfr_id  = u16::from_le_bytes([ad_content[0], ad_content[1]]);
            let payload = &ad_content[2..];
            return Some((mfr_id, payload));
        }

        i += 1 + length; // advance past this AD structure
    }
    None
}

/// Parse a raw HCI packet buffer for a LE Extended Advertising Report.
///
/// Returns an iterator-style approach: handles `num_reports` in one call,
/// returns the first report matching our MAC and company ID filter.
///
/// On match returns (mac_string, rssi, mfr_id, payload_slice).
///
/// HCI Extended Advertising Report packet layout:
///   [0]       0x04                     HCI_EVENT_PKT
///   [1]       0x3E                     LE Meta Event
///   [2]       total_param_length
///   [3]       0x0D                     LE Extended Advertising Report subevent
///   [4]       num_reports
///   [5..]     per-report data (variable length)
///
/// Per-report layout (24 bytes header + AD data):
///   [+0..1]  event_type (2B LE)
///   [+2]     primary_phy
///   [+3]     secondary_phy
///   [+4]     advertising_sid
///   [+5]     tx_power (i8)
///   [+6]     rssi (i8, 0x7F = not available)
///   [+7..8]  periodic_adv_interval (2B LE)
///   [+9]     direct_address_type
///   [+10..15] direct_address (6B)
///   [+16]    address_type
///   [+17..22] address (6B, little-endian LSB first)
///   [+23]    data_length
///   [+24..]  AD data (data_length bytes)
fn parse_ext_adv_report<'a>(
    pkt: &'a [u8],
    node_mac: &str,
) -> Option<(String, i8, u16, &'a [u8])> {
    // Minimum packet: 3B HCI header + 1B subevent + 1B num_reports
    if pkt.len() < 5 { return None; }
    if pkt[0] != 0x04                      { return None; } // not HCI_EVENT_PKT
    if pkt[1] != HCI_EVENT_CODE_LE_META    { return None; } // not LE Meta
    if pkt[3] != LE_EXT_ADV_REPORT_SUBEVENT { return None; } // not ext adv report

    let num_reports = pkt[4] as usize;
    let mut offset = 5usize;

    for _ in 0..num_reports {
        // Report header is 24 bytes (indices 0..23 relative to report start)
        const REPORT_HDR: usize = 24;
        if pkt.len() < offset + REPORT_HDR { return None; }

        let rssi_raw = pkt[offset + 13];
        let rssi = if rssi_raw == 0x7F { 0i8 } else { rssi_raw as i8 };

        // E3:79:63:12:EF:B1 → wire [B1, EF, 12, 63, 79, E3]
        let addr = &pkt[offset + 3..offset + 9];        
        let mac = format!(
            "dev_{:02X}_{:02X}_{:02X}_{:02X}_{:02X}_{:02X}",
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]
        );
        let data_length = pkt[offset + 23] as usize;
        
        if pkt.len() < offset + REPORT_HDR + data_length {
            return None;
        }
        let ad_data = &pkt[offset + REPORT_HDR..offset + REPORT_HDR + data_length];
        
        if mac != node_mac {
            offset += REPORT_HDR + data_length;
            continue;
        }
        eprintln!("[HCI] ext adv report from {}", mac);




        // MAC filter — skip to next report if not our node
        if mac != node_mac {
            offset += REPORT_HDR + data_length;
            continue;
        }
        eprintln!("[HCI] ext adv report from {}", mac);

        // Extract manufacturer specific data and apply company ID filter
        if let Some((mfr_id, payload)) = parse_ad_manufacturer(ad_data) {
            // proto_version filter: LF[0] == 0x02 → mfr_id low byte == 0x02
            if (mfr_id & 0xFF) as u8 == 0x02 {
                return Some((mac, rssi, mfr_id, payload));
            }
        }

        // Right MAC, wrong AD content — don't advance to other reports
        return None;
    }

    None
}

// ── HCI scan task ─────────────────────────────────────────────────────────────

/// Raw HCI scan loop — replaces the btleplug ble_task.
///
/// Opens AF_BLUETOOTH/BTPROTO_HCI/HCI_CHANNEL_RAW on `hci_dev`, sends LE
/// Extended Scan Parameters + Enable, then reads HCI events directly from the
/// kernel bypassing bluetoothd's D-Bus coalescing layer.
///
/// Everything downstream of the scan (verify_outer_sig, SQLite, MQTT, TUI) is
/// identical to the former btleplug implementation.
async fn hci_scan_task(
    app:        Arc<Mutex<App>>,
    conn:       Arc<Mutex<Connection>>,
    vk:         Arc<VerifyingKey>,
    mqtt_tx:    tokio::sync::mpsc::Sender<MqttFrame>,
    hci_dev:    u16,
    ntfy_topic: String,
) {
    // ── Open raw HCI socket ───────────────────────────────────────────────
    let sock_raw = hci_open_raw(hci_dev);
    if sock_raw < 0 {
        eprintln!("[HCI] failed to open raw socket — ble-stability scan task exiting");
        return;
    }
    eprintln!("[HCI] raw socket open on hci{}", hci_dev);

    // ── Send LE Extended Scan Parameters + Enable ─────────────────────────
    // Fire-and-forget: errors are logged inside hci_start_ext_scan.
    // If the adapter is already scanning (bluetoothd / Makefile pre-roll),
    // these commands are benign — they re-apply scan params and keep going.

    hci_start_ext_scan(sock_raw);

    // ── Blocking reader thread ────────────────────────────────────────────
    let (pkt_tx, mut pkt_rx) = tokio::sync::mpsc::channel::<Vec<u8>>(256);
    
    std::thread::spawn(move || {
        let mut buf = [0u8; 512];
        eprintln!("[HCI] reader thread alive");
        loop {
            let n = unsafe {
                libc::read(sock_raw, buf.as_mut_ptr() as *mut libc::c_void, buf.len())
            };
            if n < 0 {
                eprintln!("[HCI] read error: {}", std::io::Error::last_os_error());
                break;
            }
            // eprintln!("[HCI] read {} bytes pkt={:02X?}", n, &buf[..n as usize]);
            if pkt_tx.blocking_send(buf[..n as usize].to_vec()).is_err() {
                break;
            }
        }
    });

    // ── Set non-blocking, wrap in AsyncFd ────────────────────────────────
    // unsafe {
    //     let flags = libc::fcntl(sock_raw, libc::F_GETFL);
    //     libc::fcntl(sock_raw, libc::F_SETFL, flags | libc::O_NONBLOCK);
    // }

    // // SAFETY: sock_raw is a valid open fd from hci_open_raw above.
    // // OwnedFd takes ownership and will close it on drop.
    // let owned = unsafe { OwnedFd::from_raw_fd(sock_raw) };
    // let async_fd = match AsyncFd::new(owned) {
    //     Ok(fd) => fd,
    //     Err(e) => {
    //         eprintln!("[HCI] AsyncFd::new failed: {}", e);
    //         return;
    //     }
    // };
    
    // ── Async pipeline loop ───────────────────────────────────────────────
    let ntfy_client = reqwest::Client::new();
    let ntfy_url    = format!("https://ntfy.sh/{}", ntfy_topic);
    let mut last_ntfy: Option<std::time::Instant> = None;

    eprintln!("[HCI] scan loop alive — hci{} / subevent 0x0D", hci_dev);


    // Buffer large enough for any LE Extended Advertising event.
    // LF=184B + AD overhead + HCI headers ≈ 215B; 512 is comfortable.
    // let mut buf = [0u8; 512];

    eprintln!("[HCI] entering read loop");

    loop {
        // // Wait until the socket is readable
        // let mut guard = match async_fd.readable().await {
        //     Ok(g)  => g,
        //     Err(e) => {
        //         eprintln!("[HCI] readable() error: {}", e);
        //         break;
        //     }
        // };

        // let fd = guard.get_inner().as_raw_fd();
        // let n = unsafe {
        //     libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) as isize
        // };

        // eprintln!("[HCI] read {} bytes, pkt[0]={:02X} pkt[1]={:02X} pkt[3]={:02X}",
        //     n,
        //     buf.get(0).copied().unwrap_or(0),
        //     buf.get(1).copied().unwrap_or(0),
        //     buf.get(3).copied().unwrap_or(0));

        // if n < 0 {
        //     let err = std::io::Error::last_os_error();
        //     if err.raw_os_error() == Some(libc::EAGAIN)
        //         || err.raw_os_error() == Some(libc::EWOULDBLOCK)
        //     {
        //         guard.clear_ready();
        // continue;
        // }
        // eprintln!("[HCI] read error: {}", err);
        //     break;
        // }

        // let n = n as usize;

        let Some(pkt) = pkt_rx.recv().await else { break; };

        // ── Parse extended advertising report ─────────────────────────────
        let Some((mac, rssi, mfr_id, payload)) =
            parse_ext_adv_report(&pkt, NODE_MAC)
        else {
            continue;
        };

        // payload is a slice into buf — copy it before we modify buf next iteration
        let payload_vec: Vec<u8> = payload.to_vec();

        let sig_verified = verify_outer_sig(mfr_id, &payload_vec, &vk);
        let raw_blob_hex = hex::encode(&payload_vec);

        let received_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64;

        let mut rec = EventRecord {
            id: 0,
            node_id: mac.clone(),
            received_at,
            sig_verified,
            rssi,    
            frame_type: ((mfr_id >> 8) & 0xFF) as u8,
            raw_blob_hex: raw_blob_hex.clone(),
        };

        // ── DB write ──────────────────────────────────────────────────────
        {
            let db = conn.lock().await;
            match db_insert(&db, &rec) {
                Ok(id) => rec.id = id,
                Err(e) => eprintln!("DB insert error: {}", e),
            }
        }

        // ── MQTT publish — verified frames only ───────────────────────────
        if sig_verified {
            let frame = MqttFrame {
                topic:   mqtt_topic_frames(&mac),
                payload: format!(
                    r#"{{"node_id":"{}","received_at":{},"rssi":{},"lf":"{}"}}"#,
                    mac, received_at, rssi, raw_blob_hex
                ),
                retain: false, 
            };
            // try_send: non-blocking. If channel is full, log and drop — explicit policy.
            if let Err(e) = mqtt_tx.try_send(frame) {
                eprintln!("[MQTT] queue full, dropping frame: {e}");
            }
            // store to last_ntfy
            if last_ntfy.map_or(true, |t| t.elapsed().as_secs() > NTFY_DEBOUNCE_SECS) {
                tokio::spawn(ntfy_notify(ntfy_client.clone(), ntfy_url.clone()));
                last_ntfy = Some(std::time::Instant::now());
            }
        }

        // ── TUI update ────────────────────────────────────────────────────
        {
            let mut a = app.lock().await;
            a.push(rec);
        }
    }

    eprintln!("[HCI] scan loop exited — hci{} socket closed", hci_dev);
}

// ── Adapter index discovery (unchanged) ──────────────────────────────────────

fn find_realtek_hci_index() -> Option<usize> {
    let output = std::process::Command::new("hciconfig")
        .arg("-a")
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    eprintln!("[REALTEK] hciconfig output:\n{}", text);
    let mut current_idx: Option<usize> = None;
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("hci") {
            if let Some(idx_str) = rest.split(|c: char| !c.is_ascii_digit()).next() {
                current_idx = idx_str.trim().parse().ok();
                eprintln!("[REALTEK] parsed hci index: {:?}", current_idx);
            }
        }
        if line.contains("A0:AD:9F:71:13:98") {
            eprintln!("[REALTEK] found BD address on line: {:?}", line);
            return current_idx;
        }
    }
    eprintln!("[REALTEK] BD address not found in output");
    None
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {

    let headless = std::env::args().any(|a| a == "--headless");

    // ── HCI adapter index ─────────────────────────────────────────────────────
    // find_realtek_hci_index() scans hciconfig -a for the Realtek BD address
    // and returns the hci index (0, 1, ...). Falls back to 1 (known hci1 on RPi5).
    let hci_dev = match find_realtek_hci_index() {
        Some(idx) => {
            eprintln!("[LIMA] Using Realtek adapter: hci{}", idx);
            idx as u16
        }
        None => {
            eprintln!("[LIMA] WARNING: Realtek adapter not found via hciconfig, defaulting to hci1");
            1u16
        }
    };

    // ── DB init ───────────────────────────────────────────────────────────────
    let conn = Connection::open(DB_PATH)?;
    db_init(&conn)?;
    let conn = Arc::new(Mutex::new(conn));

    // ── Crypto init ───────────────────────────────────────────────────────────
    let verifying_key = Arc::new(load_test_verifying_key());

    // ── MQTT init ─────────────────────────────────────────────────────────────
    let mut mqtt_options = MqttOptions::new(MQTT_CLIENT_ID, MQTT_HOST, MQTT_PORT);
    mqtt_options.set_keep_alive(Duration::from_secs(30));

    // ── MQTT queue channel ────────────────────────────────────────────────────
    let (mqtt_tx, mqtt_rx) = tokio::sync::mpsc::channel::<MqttFrame>(128);

    tokio::spawn(mqtt_task(mqtt_rx, mqtt_options));

    eprintln!("[LIMA] MQTT task spawned — broker {}:{}", MQTT_HOST, MQTT_PORT);

    // ── App state ─────────────────────────────────────────────────────────────
    let app = Arc::new(Mutex::new(App::new()));
    let ntfy_topic = load_ntfy_topic();

    // ── Spawn raw HCI scan task ───────────────────────────────────────────────

    tokio::spawn(hci_scan_task(
        Arc::clone(&app),
        Arc::clone(&conn),
        Arc::clone(&verifying_key),
        mqtt_tx.clone(), 
        hci_dev,
        ntfy_topic,
    ));

    if headless {
        eprintln!("[LIMA] running headless — waiting for SIGTERM or SIGINT");
        let mut sigterm = signal(SignalKind::terminate())?;
        tokio::select! {
            _ = sigterm.recv()          => eprintln!("[LIMA] SIGTERM received"),
            _ = tokio::signal::ctrl_c() => eprintln!("[LIMA] SIGINT received"),
        }
    } else {

        // ── TUI setup ─────────────────────────────────────────────────────────
        enable_raw_mode()?;
        let mut stdout = io::stdout();
        execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
        let backend      = CrosstermBackend::new(stdout);
        let mut terminal = Terminal::new(backend)?;

        // ── TUI event loop ────────────────────────────────────────────────────
        loop {
            {
                let mut a = app.lock().await;
                terminal.draw(|f| ui(f, &mut a))?;
            }

            // spawn_blocking keeps crossterm's blocking poll off the async runtime
            let key = tokio::task::spawn_blocking(|| -> io::Result<Option<KeyCode>> {
                if event::poll(Duration::from_millis(100))? {
                    if let Event::Key(key) = event::read()? {
                        return Ok(Some(key.code));
                    }
                }
                Ok(None)
            }).await??;

            if key == Some(KeyCode::Char('q')) {
                break;
            }
        }

        // ── TUI teardown ──────────────────────────────────────────────────────
        disable_raw_mode()?;
        execute!(
            terminal.backend_mut(),
            LeaveAlternateScreen,
            DisableMouseCapture
        )?;
        terminal.show_cursor()?;
    }

    // ── Publish gateway offline before exit ───────────────────────────────────
    let _ = mqtt_tx.send(MqttFrame {
        topic:   MQTT_TOPIC_HEALTH.to_string(),
        payload: "offline".to_string(),
        retain:  true,
    }).await;
    // Give mqtt_task a moment to flush before the process exits
    tokio::time::sleep(Duration::from_millis(300)).await;

    println!("LIMA gateway stopped. DB saved to {}", DB_PATH);
    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────────────
//
// Add this block at the bottom of main.rs.
// Run with: cargo test -p gateway

#[cfg(test)]
mod tests {
    use super::*;

    // ── Test packet builder ───────────────────────────────────────────────────
    //
    // Constructs the exact byte sequence the kernel would write into our read
    // buffer: HCI event header → LE Meta → Extended Advertising Report.
    //
    // addr:    6-byte BLE address, little-endian (LSB first), as it appears in
    //          the HCI packet.  E3:79:63:12:EF:B1 → [0xB1,0xEF,0x12,0x63,0x79,0xE3]
    // rssi:    raw u8 (cast to i8 by parser; 0x7F = not available → 0)
    // mfr_id:  16-bit company ID, LE.  For LIMA: low byte = proto_version (0x02)
    // payload: LF[2..184], 182 bytes.  Arbitrary content for parse tests.
    fn make_ext_adv_pkt(
        addr:    [u8; 6],
        rssi:    u8,
        mfr_id:  u16,
        payload: &[u8],
    ) -> Vec<u8> {
        // ── Build AD structure (type 0xFF manufacturer specific) ──────────
        //
        // AD structure layout:
        //   [0]     length  = 1(type) + 2(company_id) + payload.len()
        //   [1]     0xFF    AD type: manufacturer specific
        //   [2]     mfr_id low byte   (= LF[0] proto_version)
        //   [3]     mfr_id high byte  (= LF[1] event_type)
        //   [4..]   payload           (= LF[2..184], 182B for LIMA)
        let ad_content_len: u8 = (1 + 2 + payload.len()) as u8; // type + cid + payload
        let mut ad: Vec<u8> = Vec::new();
        ad.push(ad_content_len);
        ad.push(0xFF);
        ad.push((mfr_id & 0xFF) as u8);
        ad.push((mfr_id >> 8) as u8);
        ad.extend_from_slice(payload);
        // ad.len() = 1 + ad_content_len = 4 + payload.len()
        // For LIMA 182B payload: ad.len() = 186

        // ── Build per-report (24-byte header + ad data) ───────────────────
        //
        // BT spec Core 5.4 Vol 4 Part E §7.7.65.13 per-report layout:
        //   [0..1]  event_type (2B LE)
        //   [2]     primary_phy
        //   [3]     secondary_phy
        //   [4]     advertising_sid
        //   [5]     tx_power (i8)
        //   [6]     rssi (i8, 0x7F = not available)
        //   [7..8]  periodic_adv_interval (2B LE)
        //   [9]     direct_address_type
        //   [10..15] direct_address (6B)
        //   [16]    address_type
        //   [17..22] address (6B LE, LSB first)
        //   [23]    data_length
        //   [24..]  AD data (data_length bytes)
        let mut report: Vec<u8> = Vec::new();
        report.extend_from_slice(&[0x13, 0x00]); // event_type: non-connectable extended
        report.push(0x01);                         // primary_phy: 1M
        report.push(0x01);                         // secondary_phy: 1M
        report.push(0xFF);                         // advertising_sid: not in a periodic set
        report.push(0x7F);                         // tx_power: not available
        report.push(rssi);                         // rssi [offset 6]
        report.extend_from_slice(&[0x00, 0x00]);   // periodic_adv_interval: 0
        report.push(0x00);                         // direct_address_type
        report.extend_from_slice(&[0x00u8; 6]);    // direct_address
        report.push(0x01);                         // address_type: random static
        report.extend_from_slice(&addr);           // address [offsets 17..22]
        report.push(ad.len() as u8);               // data_length [offset 23]
        report.extend_from_slice(&ad);             // AD data [offset 24..]

        debug_assert_eq!(report.len(), 24 + ad.len(),
            "report header+data must be 24 + ad.len()");

        // ── Build HCI event packet ────────────────────────────────────────
        //
        //   [0]    0x04  HCI_EVENT_PKT
        //   [1]    0x3E  LE Meta Event
        //   [2]    total parameter length
        //   [3]    0x0D  LE Extended Advertising Report subevent
        //   [4]    0x01  num_reports
        //   [5..]  report data
        let param_len = 2 + report.len(); // subevent(1) + num_reports(1) + report
        let mut pkt: Vec<u8> = Vec::new();
        pkt.push(0x04);
        pkt.push(0x3E);
        pkt.push(param_len as u8);
        pkt.push(0x0D);
        pkt.push(0x01);
        pkt.extend_from_slice(&report);

        pkt
    }

    // ── Shared test fixtures ──────────────────────────────────────────────────

    /// The DK's static random address as it appears in HCI wire format (LE, LSB first).
    const NODE_ADDR_LE: [u8; 6] = [0xB1, 0xEF, 0x12, 0x63, 0x79, 0xE3];

    /// Heartbeat frame: proto_version=0x02, event_type=0x04.
    const MFR_ID_HEARTBEAT: u16 = 0x0402;

    // ── parse_ext_adv_report tests ────────────────────────────────────────────

    #[test]
    fn test_round_trip_full_lima_frame() {
        // Build a synthetic 215-byte HCI packet with known payload, parse it,
        // assert every field survives the round-trip.
        let payload: Vec<u8> = (0u8..182).collect(); // 0x00..0xB5, distinct pattern
        let pkt = make_ext_adv_pkt(NODE_ADDR_LE, 0xC0, MFR_ID_HEARTBEAT, &payload);

        assert_eq!(pkt.len(), 215, "full LIMA frame should be 215 bytes");

        let result = parse_ext_adv_report(&pkt, NODE_MAC);
        assert!(result.is_some(), "should parse successfully");

        let (mac, rssi, mfr_id, got_payload) = result.unwrap();

        assert_eq!(mac,     NODE_MAC,         "MAC string");
        assert_eq!(rssi,    -64i8,            "0xC0 as i8 = -64 dBm");
        assert_eq!(mfr_id,  MFR_ID_HEARTBEAT, "company ID");
        assert_eq!(got_payload, payload.as_slice(), "payload bytes");
    }

    #[test]
    fn test_mac_filter_rejects_other_nodes() {
        // Same packet structure, different address bytes → None.
        let other_addr = [0x01u8, 0x02, 0x03, 0x04, 0x05, 0x06];
        let payload = vec![0x55u8; 182];
        let pkt = make_ext_adv_pkt(other_addr, 0xA0, MFR_ID_HEARTBEAT, &payload);
        assert!(parse_ext_adv_report(&pkt, NODE_MAC).is_none());
    }

    #[test]
    fn test_company_id_filter_rejects_wrong_proto_version() {
        // proto_version != 0x02 (low byte of mfr_id) → filtered before returning.
        let payload = vec![0x55u8; 182];
        let wrong_mfr_id: u16 = 0x0401; // proto_version = 0x01
        let pkt = make_ext_adv_pkt(NODE_ADDR_LE, 0xA0, wrong_mfr_id, &payload);
        assert!(parse_ext_adv_report(&pkt, NODE_MAC).is_none());
    }

    #[test]
    fn test_rssi_not_available_maps_to_zero() {
        // 0x7F is the HCI sentinel for "RSSI not available" → should yield 0, not 127.
        let payload = vec![0x00u8; 182];
        let pkt = make_ext_adv_pkt(NODE_ADDR_LE, 0x7F, MFR_ID_HEARTBEAT, &payload);
        let (_, rssi, _, _) = parse_ext_adv_report(&pkt, NODE_MAC).unwrap();
        assert_eq!(rssi, 0i8);
    }

    #[test]
    fn test_rssi_negative_values() {
        // Spot-check a few RSSI values across the valid range.
        let payload = vec![0x00u8; 182];
        for (raw, expected) in [(0xD8u8, -40i8), (0x81u8, -127i8), (0x14u8, 20i8)] {
            let pkt = make_ext_adv_pkt(NODE_ADDR_LE, raw, MFR_ID_HEARTBEAT, &payload);
            let (_, rssi, _, _) = parse_ext_adv_report(&pkt, NODE_MAC).unwrap();
            assert_eq!(rssi, expected, "raw=0x{:02X}", raw);
        }
    }

    #[test]
    fn test_truncated_packets_do_not_panic() {
        let full = make_ext_adv_pkt(NODE_ADDR_LE, 0xC0, MFR_ID_HEARTBEAT, &vec![0u8; 182]);
        // Trim the packet to every length from 0 to 28 (just before AD data starts).
        for len in 0..29 {
            let truncated = &full[..len];
            // Must not panic; None is the correct result for any truncated input.
            assert!(parse_ext_adv_report(truncated, NODE_MAC).is_none(),
                "should return None for len={}", len);
        }
    }

    #[test]
    fn test_wrong_hci_event_code_ignored() {
        // 0x04 type + non-LE-Meta event code → None (e.g. Command Complete 0x0E)
        let pkt = [0x04u8, 0x0E, 0x04, 0x01, 0x01, 0x20, 0x00];
        assert!(parse_ext_adv_report(&pkt, NODE_MAC).is_none());
    }

    #[test]
    fn test_wrong_subevent_ignored() {
        // LE Meta + subevent 0x02 (legacy LE Advertising Report) → None.
        let pkt = [0x04u8, 0x3E, 0x05, 0x02, 0x01, 0x00, 0x00, 0x00];
        assert!(parse_ext_adv_report(&pkt, NODE_MAC).is_none());
    }

    // ── parse_ad_manufacturer tests ───────────────────────────────────────────

    #[test]
    fn test_parse_ad_manufacturer_basic() {
        // Single AD structure, type 0xFF, 2-byte company ID + 4-byte payload.
        let mfr_id: u16 = 0x0402;
        let data = [0xDE, 0xAD, 0xBE, 0xEF];
        let mut ad: Vec<u8> = vec![
            1 + 2 + 4,  // length: type(1) + cid(2) + data(4)
            0xFF,
            0x02, 0x04, // mfr_id LE
        ];
        ad.extend_from_slice(&data);

        let result = parse_ad_manufacturer(&ad);
        assert!(result.is_some());
        let (got_mfr_id, got_payload) = result.unwrap();
        assert_eq!(got_mfr_id, mfr_id);
        assert_eq!(got_payload, data.as_slice());
    }

    #[test]
    fn test_parse_ad_manufacturer_skips_other_types() {
        // Complete Local Name (0x09) followed by Manufacturer Specific (0xFF).
        let mut ad: Vec<u8> = vec![
            4,           // length: 1(type) + 3(data)
            0x09,        // Complete Local Name
            b'D', b'K', b'1',
        ];
        ad.push(1 + 2 + 2); // 0xFF AD length
        ad.push(0xFF);
        ad.extend_from_slice(&[0x02, 0x04, 0xAA, 0xBB]);

        let (mfr_id, payload) = parse_ad_manufacturer(&ad).unwrap();
        assert_eq!(mfr_id, 0x0402);
        assert_eq!(payload, &[0xAA, 0xBB]);
    }

    #[test]
    fn test_parse_ad_manufacturer_empty_returns_none() {
        assert!(parse_ad_manufacturer(&[]).is_none());
        assert!(parse_ad_manufacturer(&[0x00]).is_none()); // zero-length terminator
    }

    #[test]
    fn test_parse_ad_manufacturer_no_0xff_returns_none() {
        let ad = [0x02u8, 0x01, 0x06]; // Flags AD type (0x01)
        assert!(parse_ad_manufacturer(&ad).is_none());
    }

    // ── MAC byte order test ───────────────────────────────────────────────────

    #[test]
    fn test_mac_byte_reversal() {
        // BLE wire format: [LSB...MSB].  E3:79:63:12:EF:B1 on wire = [B1,EF,12,63,79,E3].
        // Parser must reverse to produce dev_E3_79_63_12_EF_B1.
        let addr = [0xB1u8, 0xEF, 0x12, 0x63, 0x79, 0xE3];
        let mac = format!(
            "dev_{:02X}_{:02X}_{:02X}_{:02X}_{:02X}_{:02X}",
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]
        );
        assert_eq!(mac, "dev_E3_79_63_12_EF_B1");
        assert_eq!(mac, NODE_MAC);
    }

    // ── Struct layout test ────────────────────────────────────────────────────

    #[test]
    fn test_hci_filter_is_packed_14_bytes() {
        // If repr(C, packed) is accidentally removed, setsockopt will pass the
        // wrong length to the kernel and the filter will be silently misconfigured.
        assert_eq!(
            std::mem::size_of::<HciFilter>(), 14,
            "HciFilter must be packed 14 bytes: u32(4) + [u32;2](8) + u16(2)"
        );
    }

    #[test]
    fn test_sock_addr_hci_is_6_bytes() {
        assert_eq!(std::mem::size_of::<SockAddrHci>(), 6);
    }
}