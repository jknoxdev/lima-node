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

use btleplug::api::{Central, Manager as _, Peripheral, ScanFilter};
use btleplug::platform::Manager;
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

use lima_types::{NONCE_LEN, TAG_LEN, OUTER_SIG_LEN};

// ── Constants ─────────────────────────────────────────────────────────────────

/// Hardcoded test verifying key (DER/SEC1 uncompressed, 65 bytes)
/// Replace with provisioned key store in next sprint.
/// This is the node verifying key from crypto-test ephemeral run —
/// swap with your real node pubkey bytes when testing against firmware.
const TEST_NODE_PUBKEY_HEX: &str = concat!(
    "04",
    "82c776fe1af3e080e0d33a0b5968e6",
    "bd933791097e3aa282cd6efa72ad093a",
    "fd7550d59963ba232bb5e81e3c79a96d",
    "637cd655affbee83c6ac401d395c98a3",
    "35"
);

const DB_PATH: &str = "lima_gateway.db";
const LIMA_ADV_PREFIX: &str = "LIMA";

// ── Event record ──────────────────────────────────────────────────────────────

#[derive(Clone)]
struct EventRecord {
    id:           i64,
    node_id:      String,
    received_at:  u64,
    sig_verified: bool,
    event_type:   String,
    rssi:         i16,
    raw_blob_hex: String,
}

// ── App state ─────────────────────────────────────────────────────────────────

struct App {
    events:      Vec<EventRecord>,
    table_state: TableState,
    total_rx:    u64,
    total_valid: u64,
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
            event_type    TEXT    NOT NULL,
            rssi          INTEGER NOT NULL,
            raw_blob      BLOB    NOT NULL
        );",
    )
}

fn db_insert(conn: &Connection, rec: &EventRecord) -> rusqlite::Result<i64> {
    conn.execute(
        "INSERT INTO events
            (node_id, received_at, sig_verified, event_type, rssi, raw_blob)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            rec.node_id,
            rec.received_at,
            rec.sig_verified as i32,
            rec.event_type,
            rec.rssi,
            rec.raw_blob_hex,
        ],
    )?;
    Ok(conn.last_insert_rowid())
}

// ── Crypto ────────────────────────────────────────────────────────────────────

fn load_test_verifying_key() -> VerifyingKey {
    let bytes = hex::decode(TEST_NODE_PUBKEY_HEX.replace("\n", ""))
        .expect("TEST_NODE_PUBKEY_HEX invalid hex");
    VerifyingKey::from_sec1_bytes(&bytes)
        .expect("TEST_NODE_PUBKEY_HEX invalid P-256 key")
}

/// Verify outer ECDSA signature over (nonce || ciphertext || tag)
/// Wire format: [nonce(12) | ciphertext(88) | tag(16) | outer_sig(64)]
fn verify_outer_sig(payload: &[u8], vk: &VerifyingKey) -> bool {
    let min_len = NONCE_LEN + TAG_LEN + OUTER_SIG_LEN + 1;
    if payload.len() < min_len {
        return false;
    }

    let sig_offset = payload.len() - OUTER_SIG_LEN;
    let signed_data = &payload[..sig_offset];
    let sig_bytes   = &payload[sig_offset..];

    match Signature::from_slice(sig_bytes) {
        Ok(sig) => vk.verify(signed_data, &sig).is_ok(),
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
    let header_cells = ["time", "node_id", "type", "seq", "sig", "rssi"]
        .iter()
        .map(|h| Cell::from(*h).style(Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)));
    let table_header = Row::new(header_cells).height(1).bottom_margin(1);

    let rows = app.events.iter().map(|rec| {
        let sig_cell = if rec.sig_verified {
            Cell::from("✅ VALID").style(Style::default().fg(Color::Green))
        } else {
            Cell::from("❌ INVALID").style(Style::default().fg(Color::Red).add_modifier(Modifier::BOLD))
        };

        let time_str = format_timestamp(rec.received_at);

        Row::new(vec![
            Cell::from(time_str),
            Cell::from(rec.node_id.clone()),
            Cell::from(rec.event_type.clone()),
            Cell::from("—"),   // seq: not decoded until AES decrypt sprint
            sig_cell,
            Cell::from(format!("{} dBm", rec.rssi)),
        ])
        .height(1)
    });

    let table = Table::new(
        rows,
        [
            Constraint::Length(12),  // time
            Constraint::Length(20),  // node_id
            Constraint::Length(14),  // type
            Constraint::Length(8),   // seq
            Constraint::Length(12),  // sig
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
    let footer = Block::default()
        .borders(Borders::ALL)
        .title(" q: quit  |  DB: lima_gateway.db  |  skeleton: no AES decrypt yet ")
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

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // ── DB init ───────────────────────────────────────────────────────────────
    let conn = Connection::open(DB_PATH)?;
    db_init(&conn)?;
    let conn = Arc::new(Mutex::new(conn));

    // ── Crypto init ───────────────────────────────────────────────────────────
    let verifying_key = Arc::new(load_test_verifying_key());

    // ── App state ─────────────────────────────────────────────────────────────
    let app = Arc::new(Mutex::new(App::new()));

    // ── BLE scanner task ──────────────────────────────────────────────────────
    let app_ble  = Arc::clone(&app);
    let conn_ble = Arc::clone(&conn);
    let vk_ble   = Arc::clone(&verifying_key);

    tokio::spawn(async move {
        let manager  = Manager::new().await.expect("BLE manager failed");
        let adapters = manager.adapters().await.expect("No BLE adapters");
        let adapter  = adapters.into_iter().next().expect("No BLE adapter found");

        adapter.start_scan(ScanFilter::default()).await
            .expect("BLE scan failed");

        loop {
            let peripherals = adapter.peripherals().await.unwrap_or_default();

            for p in peripherals {
                let props = match p.properties().await {
                    Ok(Some(p)) => p,
                    _ => continue,
                };

                let is_lima = props.local_name
                    .as_deref()
                    .map(|n| n.starts_with(LIMA_ADV_PREFIX))
                    .unwrap_or(false);

                if !is_lima {
                    continue;
                }

                let node_id = props.address.to_string();
                let rssi    = props.rssi.unwrap_or(0);

                for (_, bytes) in &props.manufacturer_data {
                    let sig_verified = verify_outer_sig(bytes, &vk_ble);
                    let raw_blob_hex = hex::encode(bytes);

                    // Best-effort event type from first byte after potential header
                    // Real decode happens after AES decrypt sprint
                    let event_type = if bytes.len() > 4 {
                        format!("0x{:02X}", bytes[4])
                    } else {
                        "unknown".to_string()
                    };

                    let received_at = SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as u64;

                    let mut rec = EventRecord {
                        id: 0,
                        node_id: node_id.clone(),
                        received_at,
                        sig_verified,
                        event_type,
                        rssi,
                        raw_blob_hex,
                    };

                    // Store to SQLite
                    {
                        let db = conn_ble.lock().await;
                        match db_insert(&db, &rec) {
                            Ok(id) => rec.id = id,
                            Err(e) => eprintln!("DB insert error: {}", e),
                        }
                    }

                    // Update TUI state
                    {
                        let mut a = app_ble.lock().await;
                        a.push(rec);
                    }
                }
            }

            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    });

    // ── TUI setup ─────────────────────────────────────────────────────────────
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    let backend  = CrosstermBackend::new(stdout);
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