//! LIMA Gateway — BLE scanner, signature verification, SQLite audit log, ratatui TUI
//!
//! Pipeline:
//!   btleplug scan → extract manufacturer payload → verify outer ECDSA sig
//!   → store raw encrypted blob in SQLite → publish to MQTT → update ratatui TUI
//!
//! Skeleton: uses hardcoded test verifying key from crypto-test.
//! Real provisioning (key store + AES decrypt) is next sprint.

use std::{
    io,
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use btleplug::api::{Central, Manager as _, Peripheral, ScanFilter};
use btleplug::platform::{Adapter, Manager};
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
use lima_types::{LF_LEN, LF_SIGNED_BYTES, LF_OFFSET_OUTER_SIG, OUTER_SIG_LEN};
use rumqttc::{AsyncClient, MqttOptions, QoS};
use dirs;


// ── Constants ─────────────────────────────────────────────────────────────────

// Current node public key — provisioned 2026-03-28
// [00:00:06.106,262] <inf> lima_crypto: CRYPTO: ECDSA public key (65 bytes):
//   04 e5 cb a4 c8 55 04 fc  25 ca 64 21 5f 89 5d 48
//   b7 87 13 98 d2 37 d9 62  1a 49 7d bd b4 7b 94 d1
//   f1 98 ff ff f9 8b 9d 0a  1c a6 9f f7 cb 36 90 99
//   8e 2f a4 5e 86 03 50 72  d9 3e c7 9f d6 c7 23 e2
//   75
const TEST_NODE_PUBKEY_HEX: &str = concat!(
    "04 e5 cb a4 c8 55 04 fc  25 ca 64 21 5f 89 5d 48 ",
    "b7 87 13 98 d2 37 d9 62  1a 49 7d bd b4 7b 94 d1 ",
    "f1 98 ff ff f9 8b 9d 0a  1c a6 9f f7 cb 36 90 99 ",
    "8e 2f a4 5e 86 03 50 72  d9 3e c7 9f d6 c7 23 e2 ",
    "75"
);

const DB_PATH:   &str = "lima_gateway.db";
const NODE_MAC:  &str = "hci1/dev_E3_79_63_12_EF_B1";

// ── MQTT constants ────────────────────────────────────────────────────────────

const MQTT_HOST:      &str = "localhost";
const MQTT_PORT:      u16  = 1883;
const MQTT_CLIENT_ID: &str = "lima-gateway";
const NTFY_DEBOUNCE_SECS: u64 = 5;
// const NTFY_TOPIC: &str = "333da315460b794864ff39565ab0eb777598f6000839c67ff4fb77f73c03345f"; # no oops

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
    // sanitize btleplug node_id ("hci1/dev_E3_79_63_12_EF_B1") for MQTT topic
    let clean = node_id.replace('/', "-").replace('_', "-");
    format!("lima/nodes/{}/frames", clean)
}

// ── MQTT frame to publish ─────────────────────────────────────────────────
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
/// btleplug strips LF[0] (proto_version) and LF[1] (event_type) into the
/// mfr_id HashMap key. Payload arrives as 182 bytes: LF[2..183].
/// Reconstruct full 184B LF then verify outer_sig over LF[0..120].
///
/// LF layout (184B):
///   [0]       proto_version   ← stripped into mfr_id low byte
///   [1]       event_type      ← stripped into mfr_id high byte
///   [2-3]     reserved
///   [4-15]    nonce (12B)
///   [16-103]  ciphertext (88B) — opaque, never inspected here
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
        // LF payload offsets (182B, company_id stripped by btleplug):
        // [0]       proto_version  [1] event_type  [2-3] reserved
        // [4-15]    nonce          [16-103] ciphertext (seq/timestamp inside, encrypted)
        // [104-119] gcm_tag        [120-181] outer_sig
        let raw = hex::decode(&rec.raw_blob_hex).unwrap_or_default();

        let evt = raw.get(1)
            .map(|b| format!("0x{:02X}", b))
            .unwrap_or_else(|| "?".to_string());

        // seq is inside ciphertext — not visible without decryption
        let seq = "--".to_string();

        // outer_sig starts at offset 120 in 182B payload
        let sig_fp = if raw.len() >= 124 {
            format!("{:02X}{:02X}{:02X}{:02X}",
                raw[120], raw[121], raw[122], raw[123])
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
        let sig_fp = if raw.len() >= 124 {
            format!("{:02X}{:02X}{:02X}{:02X}",
                raw[120], raw[121], raw[122], raw[123])
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

// ── MQTT task — owns retry + reconnect logic ──────────────────────────────
async fn mqtt_task(
    mut rx:       tokio::sync::mpsc::Receiver<MqttFrame>,
    mqtt_options: MqttOptions,
) {
    const RETRY_DELAY:     Duration = Duration::from_secs(5);
    const PUBLISH_TIMEOUT: Duration = Duration::from_secs(5);

    loop {
        // (Re)connect
        let (client, mut eventloop) = AsyncClient::new(mqtt_options.clone(), 64);

        // oneshot — eventloop sub-task signals publish loop the moment connection dies
        let (dead_tx, mut dead_rx) = tokio::sync::oneshot::channel::<()>();

        tokio::spawn(async move {
            loop {
                match eventloop.poll().await {
                    Ok(_)  => {}
                    Err(e) => {
                        eprintln!("[MQTT] eventloop error: {e}");
                        let _ = dead_tx.send(());
                        return; // exit cleanly, don't retry here
                    }
                }
            }
        });

        // Online health publish
        let _ = client.publish(MQTT_TOPIC_HEALTH, QoS::AtLeastOnce, true, "online").await;
        eprintln!("[MQTT] connected to broker {}:{}", MQTT_HOST, MQTT_PORT);

        // Drain incoming frames until connection dies
        'publish: loop {
            tokio::select! {
                // connection died — break immediately, don't wait for publish timeout
                _ = &mut dead_rx => {
                    eprintln!("[MQTT] connection lost — reconnecting");
                    break 'publish;
                }

                frame = rx.recv() => {
                    match frame {
                        None => return, // sender dropped — gateway shutting down
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

// ── BLE task ──────────────────────────────────────────────────────────────────
async fn ble_task(
    app:     Arc<Mutex<App>>,
    conn:    Arc<Mutex<Connection>>,
    vk:      Arc<VerifyingKey>,
    mqtt_tx: tokio::sync::mpsc::Sender<MqttFrame>, 
    adapter: Adapter,
    ntfy_topic: String,
) {
    use btleplug::api::CentralEvent;
    use futures::StreamExt;

    adapter.start_scan(ScanFilter::default()).await
        .expect("BLE scan failed");

    let mut events = adapter.events().await
        .expect("Failed to get BLE event stream");

    let ntfy_client = reqwest::Client::new();
    let ntfy_url = format!("https://ntfy.sh/{}", ntfy_topic);
    let mut last_ntfy: Option<std::time::Instant> = None;
    eprintln!("[LIMA] BLE event loop alive");

    while let Some(event) = events.next().await {
        // ── Extract id + manufacturer_data, discard everything else ──────────
        let (peripheral_id, manufacturer_data) = match event {
            CentralEvent::ManufacturerDataAdvertisement { id, manufacturer_data } => {
                (id, manufacturer_data)
            }
            _ => continue,
        };

        // ── MAC filter FIRST — before any btleplug calls ──────────────────
        let address = peripheral_id.to_string();
        if address != NODE_MAC {
            continue;
        }
        eprintln!("[LIMA] event received");

        // ── RSSI lookup only for LIMA node ────────────────────────────────
        let rssi = match adapter.peripheral(&peripheral_id).await {
            Ok(p) => match p.properties().await {
                Ok(Some(props)) => props.rssi.unwrap_or(0) as i8,
                _ => 0i8,
            },
            _ => 0i8,
        };

        // ── proto_version filter ──────────────────────────────────────────
        let Some((mfr_id, bytes)) = manufacturer_data.iter()
            .find(|(id, _)| (*id & 0xFF) as u8 == 0x02)
        else {
            continue;
        };

        let sig_verified = verify_outer_sig(*mfr_id, bytes, &vk);
        let raw_blob_hex = hex::encode(bytes);

        let received_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64;

        let mut rec = EventRecord {
            id: 0,
            node_id: address.clone(),
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

        // ── MQTT publish — verified frames only, with timeout ─────────────
        if sig_verified {
            let frame = MqttFrame {
                topic:   mqtt_topic_frames(&address),
                payload: format!(
                    r#"{{"node_id":"{}","received_at":{},"rssi":{},"lf":"{}"}}"#,
                    address, received_at, rssi, raw_blob_hex
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
                last_ntfy = Some(std::time::Instant::now());  // ← add this back
            }

        }


        // ── TUI update ────────────────────────────────────────────────────
        {
            let mut a = app.lock().await;
            a.push(rec);
        }
    }

    eprintln!("[LIMA] BLE event stream ended — adapter disconnected?");
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {

    // ── BLE adapter discovery ─────────────────────────────────────────────────
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

    // ── MQTT init ─────────────────────────────────────────────────────────────
    let mut mqtt_options = MqttOptions::new(MQTT_CLIENT_ID, MQTT_HOST, MQTT_PORT);
    mqtt_options.set_keep_alive(Duration::from_secs(30));


    // ── MQTT queue channel ────────────────────────────────────────────────────
    let (mqtt_tx, mqtt_rx) = tokio::sync::mpsc::channel::<MqttFrame>(128);

    tokio::spawn(mqtt_task(mqtt_rx, mqtt_options));

    eprintln!("[LIMA] MQTT connected — broker {}:{}", MQTT_HOST, MQTT_PORT);

    // ── App state ─────────────────────────────────────────────────────────────
    let app = Arc::new(Mutex::new(App::new()));
    let ntfy_topic = load_ntfy_topic();

    // ── Spawn BLE task ────────────────────────────────────────────────────────
    tokio::spawn(ble_task(
        Arc::clone(&app),
        Arc::clone(&conn),
        Arc::clone(&verifying_key),
        mqtt_tx.clone(), 
        adapter,
        ntfy_topic,
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

    // ── TUI teardown ──────────────────────────────────────────────────────────
    disable_raw_mode()?;
    execute!(
        terminal.backend_mut(),
        LeaveAlternateScreen,
        DisableMouseCapture
    )?;
    terminal.show_cursor()?;

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
