//! LIMA Gateway — BLE scanner, signature verification, SQLite audit log, ratatui TUI
//!
//! Pipeline:
//!   btleplug scan → extract manufacturer payload → verify outer ECDSA sig
//!   → store raw encrypted blob in SQLite → update ratatui TUI
//!
//! Skeleton: uses hardcoded test verifying key from crypto-test.
//! Real provisioning (key store + ECDH + AES decrypt) is next sprint.

use std::{
    io,
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use btleplug::api::{Central, CentralEvent, Manager as _, Peripheral, ScanFilter};
use btleplug::platform::{Adapter, Manager};
use futures::StreamExt;

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

// ── Constants ─────────────────────────────────────────────────────────────────

// [00:00:06.075,622] <inf> lima_crypto: CRYPTO: existing key found at slot 0x00000001
// [00:00:06.098,968] <inf> lima_crypto: CRYPTO: public key (65 bytes):
// [00:00:06.098,999] <inf> lima_crypto:   pubkey:
//                                       04 8d a8 7d 0a 4d df c4  16 c4 01 82 6e d8 ea 0d |...}.M.. ....n...
//                                       b2 9e c3 65 13 50 69 69  b8 8c 83 79 de 06 e3 10 |...e.Pii ...y....
//                                       3e 42 a3 9e 66 e8 f3 e7  aa 62 d2 aa 24 18 4d 88 |>B..f... .b..$.M.
//                                       e1 1f 2c 7a aa 9d e8 a0  48 84 90 5b 59 ed 48 7f |..,z.... H..[Y.H.
//                                       d7                                               |.
// [00:00:06.099,029] <inf> lima_crypto: CRYPTO: initialized — ECDSA-P256/SHA-256 ready (key_id=0x00000001)

const TEST_NODE_PUBKEY_HEX: &str = concat!(
    "04 8d a8 7d 0a 4d df c4  16 c4 01 82 6e d8 ea 0d ",
    "b2 9e c3 65 13 50 69 69  b8 8c 83 79 de 06 e3 10 ",
    "3e 42 a3 9e 66 e8 f3 e7  aa 62 d2 aa 24 18 4d 88 ",
    "e1 1f 2c 7a aa 9d e8 a0  48 84 90 5b 59 ed 48 7f ",
    "d7"
);

const DB_PATH: &str = "lima_gateway.db";

// ── Event record ──────────────────────────────────────────────────────────────

#[derive(Clone)]
struct EventRecord {
    id:           i64,
    node_id:      String,
    received_at:  u64,
    sig_verified: bool,
    rssi:         i8,
    raw_blob_hex: String,
}

// ── App state ─────────────────────────────────────────────────────────────────

struct App {
    events:        Vec<EventRecord>,
    table_state:   TableState,
    total_rx:      u64,
    total_valid:   u64,
    total_invalid: u64,
}

impl App {
    fn new() -> Self {
        Self {
            events:        Vec::new(),
            table_state:   TableState::default(),
            total_rx:      0,
            total_valid:   0,
            total_invalid: 0,
        }
    }

    fn push(&mut self, rec: EventRecord) {
        self.total_rx += 1;
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
    conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS events (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id       TEXT    NOT NULL,
            received_at   INTEGER NOT NULL,
            sig_verified  INTEGER NOT NULL,
            rssi          INTEGER NOT NULL,
            raw_blob      BLOB    NOT NULL
        );",
    )
}

fn db_insert(conn: &Connection, rec: &EventRecord) -> rusqlite::Result<i64> {
    conn.execute(
        "INSERT INTO events
            (node_id, received_at, sig_verified, rssi, raw_blob)
         VALUES (?1, ?2, ?3, ?4, ?5)",
        params![
            rec.node_id,
            rec.received_at,
            rec.sig_verified as i32,
            rec.rssi,
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

/// Wire format on the wire (90 bytes total, per ble.h lima_adv_payload_t):
/// [company_id(2) | proto_version(1) | event_type(1) | sequence(4) |
///  timestamp_ms(4) | accel_g(4) | delta_pa(4) | node_id(6) | sig(64)]
///
/// btleplug strips company_id into the HashMap key — buffer arrives as 88 bytes:
/// [proto_version(1) | event_type(1) | sequence(4) | timestamp_ms(4) |
///  accel_g(4) | delta_pa(4) | node_id(6) | sig(64)]
///
/// Signed data is lima_payload_t (24 bytes), reconstructed as firmware built it:
/// [node_id(6) | event_type(1) | reserved(1) | sequence(4) |
///  timestamp_ms(4) | accel_g(4) | delta_pa(4)]
fn verify_outer_sig(payload: &[u8], vk: &VerifyingKey) -> bool {
    // 90 bytes on wire minus 2-byte company_id stripped by btleplug
    const ADV_LEN:     usize = 88;
    const PAYLOAD_LEN: usize = 24;

    if payload.len() < ADV_LEN {
        return false;
    }

    // Offsets after company_id strip — confirmed against ble.h
    let event_type   = payload[1];
    let sequence     = &payload[2..6];
    let timestamp_ms = &payload[6..10];
    let accel_g      = &payload[10..14];
    let delta_pa     = &payload[14..18];
    let node_id      = &payload[18..24];
    let sig_bytes    = &payload[24..88];

    // Reconstruct lima_payload_t (24 bytes) exactly as firmware built it
    let mut signed_data = [0u8; PAYLOAD_LEN];
    signed_data[0..6].copy_from_slice(node_id);        // node_id
    signed_data[6]    = event_type;                     // event_type
    signed_data[7]    = 0x00;                           // reserved
    signed_data[8..12].copy_from_slice(sequence);       // sequence
    signed_data[12..16].copy_from_slice(timestamp_ms);  // timestamp_ms
    signed_data[16..20].copy_from_slice(accel_g);       // accel_g
    signed_data[20..24].copy_from_slice(delta_pa);      // delta_pa

    match Signature::from_slice(sig_bytes) {
        Ok(sig) => vk.verify(&signed_data, &sig).is_ok(),
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
        // Parse evt, seq, sig fingerprint from raw bytes
        // Offsets confirmed against ble.h (company_id stripped by btleplug):
        // [0] proto_version  [1] event_type  [2..6] sequence  [24..88] sig
        let raw = hex::decode(&rec.raw_blob_hex).unwrap_or_default();

        let evt = raw.get(1)
            .map(|b| format!("0x{:02X}", b))
            .unwrap_or_else(|| "?".to_string());

        let seq = if raw.len() >= 6 {
            u32::from_le_bytes([raw[2], raw[3], raw[4], raw[5]]).to_string()
        } else {
            "?".to_string()
        };

        let sig_fp = if raw.len() >= 28 {
            // sig starts at offset 24 — show first 4 bytes as fingerprint
            format!("{:02X}{:02X}{:02X}{:02X}",
                raw[24], raw[25], raw[26], raw[27])
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

    f.render_stateful_widget(table, chunks[1], &mut app.table_state);

    // ── Footer ────────────────────────────────────────────────────────────────
    let last = app.events.first().map(|e| {
        let raw = hex::decode(&e.raw_blob_hex).unwrap_or_default();
        let sig_fp = if raw.len() >= 28 {
            format!("{:02X}{:02X}{:02X}{:02X}",
                raw[24], raw[25], raw[26], raw[27])
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
        " q: quit  |  DB: {}  |  last: {}  |  skeleton: no AES decrypt yet ",
        DB_PATH, last
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

// ── BLE task ──────────────────────────────────────────────────────────────────
async fn ble_task(
    app:     Arc<Mutex<App>>,
    conn:    Arc<Mutex<Connection>>,
    vk:      Arc<VerifyingKey>,
    adapter: Adapter,
) {
    use btleplug::api::CentralEvent;
    use futures::StreamExt;

    adapter.start_scan(ScanFilter::default()).await
        .expect("BLE scan failed");

    let mut events = adapter.events().await
        .expect("Failed to get BLE event stream");

    while let Some(event) = events.next().await {
        let (address, manufacturer_data, rssi) = match event {
            CentralEvent::ManufacturerDataAdvertisement {
                id,
                manufacturer_data,
            } => {
                // Get RSSI from peripheral properties
                let rssi = match adapter.peripheral(&id).await {
                    Ok(p) => match p.properties().await {
                        Ok(Some(props)) => props.rssi.unwrap_or(0) as i8,
                        _ => 0i8,
                    },
                    _ => 0i8,
                };
                (id.to_string(), manufacturer_data, rssi)
            }
            _ => continue,
        };

        // Filter to LIMA manufacturer ID (0xFFFF)
        let Some(bytes) = manufacturer_data.get(&0xFFFF) else {
            continue;
        };

        let sig_verified = verify_outer_sig(bytes, &vk);
        let raw_blob_hex = hex::encode(bytes);

        let received_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64;

        let mut rec = EventRecord {
            id: 0,
            node_id: address,
            received_at,
            sig_verified,
            rssi,
            raw_blob_hex,
        };

        {
            let db = conn.lock().await;
            match db_insert(&db, &rec) {
                Ok(id) => rec.id = id,
                Err(e) => eprintln!("DB insert error: {}", e),
            }
        }

        {
            let mut a = app.lock().await;
            a.push(rec);
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {

    // ── BLE adapter discovery (before TUI — output visible in terminal) ───────
    eprintln!("[LIMA] Scanning for BLE adapters...");
    let manager  = Manager::new().await.expect("BLE manager failed");
    let adapters = manager.adapters().await.expect("Failed to list BLE adapters");

    if adapters.is_empty() {
        eprintln!("[LIMA] ERROR: no BLE adapters found.");
        eprintln!("[LIMA] Check: sudo systemctl status bluetooth");
        eprintln!("[LIMA] Check: sudo setcap 'cap_net_raw,cap_net_admin+eip' target/debug/gateway");
        std::process::exit(1);
    }

    let mut adapter_infos = Vec::new();
    for (i, a) in adapters.iter().enumerate() {
        let info = a.adapter_info().await.unwrap_or_else(|_| "unknown".to_string());
        eprintln!("[LIMA]   adapter {}: {}", i, info);
        adapter_infos.push(info);
    }

    // Prefer hci1 (ASUS BT500 — required for BLE 5.0 extended adv)
    // hci0 is onboard Cypress chip, cannot receive extended advertisements
    let adapter_idx = adapter_infos.iter()
        .position(|info| info.contains("hci1"))
        .unwrap_or_else(|| {
            eprintln!("[LIMA] WARNING: hci1 not found, falling back to adapter 0");
            0
        });

    let adapter = adapters.into_iter().nth(adapter_idx)
        .expect("No BLE adapter found");

    eprintln!("[LIMA] Using adapter {}: {}", adapter_idx, adapter_infos[adapter_idx]);

    // ── DB init ───────────────────────────────────────────────────────────────
    let conn = Connection::open(DB_PATH)?;
    db_init(&conn)?;
    let conn = Arc::new(Mutex::new(conn));

    // ── Crypto init ───────────────────────────────────────────────────────────
    let verifying_key = Arc::new(load_test_verifying_key());

    // ── App state ─────────────────────────────────────────────────────────────
    let app = Arc::new(Mutex::new(App::new()));

    // ── Spawn BLE task with selected adapter ──────────────────────────────────
    tokio::spawn(ble_task(
        Arc::clone(&app),
        Arc::clone(&conn),
        Arc::clone(&verifying_key),
        adapter,
    ));

    // ── TUI setup ─────────────────────────────────────────────────────────────
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    let backend      = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    // ── TUI event loop ────────────────────────────────────────────────────────
    loop {
        {
            let mut a = app.lock().await;
            terminal.draw(|f| ui(f, &mut a))?;
        }

        if event::poll(Duration::from_millis(100))? {
            if let Event::Key(key) = event::read()? {
                if key.code == KeyCode::Char('q') {
                    break;
                }
            }
        }
    }

    // ── TUI teardown ──────────────────────────────────────────────────────────
    disable_raw_mode()?;
    execute!(
        terminal.backend_mut(),
        LeaveAlternateScreen,
        DisableMouseCapture
    )?;
    terminal.show_cursor()?;

    println!("LIMA gateway stopped. DB saved to {}", DB_PATH);
    Ok(())
}