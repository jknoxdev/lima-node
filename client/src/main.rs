use std::{path::PathBuf, time::Duration};

use crossterm::{
    event::{self, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{backend::CrosstermBackend, Terminal};

mod crypto;
mod db;
mod display;

use crypto::decrypt_ler;
use db::{open_db, poll_new_frames};
use display::{draw_ui, AppState};
use anyhow::Context;


/// Default path to the gateway's SQLite database.
/// Override with LIMA_DB env var if the path differs.
const DEFAULT_DB_PATH: &str = "/home/arx/lima-node/gateway/lima_gateway.db";

/// How often to poll the DB for new frames (ms).
const POLL_INTERVAL_MS: u64 = 500;

/// Decode and validate a hex PSK string into a 32-byte key.
/// Accepts optional whitespace around the input (e.g. from prompt_password).
pub fn parse_psk(raw: &str) -> anyhow::Result<[u8; 32]> {
    let trimmed = raw.trim();
    let bytes = hex::decode(trimmed)
        .map_err(|_| anyhow::anyhow!("PSK is not valid hex"))?;
    anyhow::ensure!(
        bytes.len() == 32,
        "PSK must be 32 bytes (64 hex chars), got {}",
        bytes.len()
    );
    Ok(bytes.try_into().unwrap())
}

/// Resolve the DB path — LIMA_DB env var overrides the compiled-in default.
pub fn resolve_db_path() -> PathBuf {
    std::env::var("LIMA_DB")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(DEFAULT_DB_PATH))
}



fn main() -> anyhow::Result<()> {
    // --- 1. Prompt for PSK (no terminal echo) ---
    let psk_hex = rpassword::prompt_password("LIMA PSK (hex): ")?;
    let key = parse_psk(&psk_hex)?;
    eprintln!("✓  PSK accepted (32 bytes)");          // visible before TUI takes over

    // --- 2. Open gateway SQLite DB ---
    let db_path = resolve_db_path();
    eprintln!("   DB  → {}", db_path.display());

    let conn = open_db(&db_path)
        .with_context(|| format!("failed to open DB at {}", db_path.display()))?;

    db::check_schema(&conn)?;
    eprintln!("✓  schema OK — entering TUI\n");

    // --- 3. Set up ratatui terminal ---
    enable_raw_mode()?;
    let mut stdout = std::io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut app = AppState::new();
    let mut last_rowid: i64 = db::last_rowid(&conn).unwrap_or(0);

    // --- 4. Main event + poll loop ---
    let result = run_loop(&mut terminal, &mut app, &conn, &key, &mut last_rowid);

    // --- 5. Restore terminal regardless of how we exit ---
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;

    result
}

fn run_loop(
    terminal: &mut Terminal<CrosstermBackend<std::io::Stdout>>,
    app: &mut AppState,
    conn: &rusqlite::Connection,
    key: &[u8; 32],
    last_rowid: &mut i64,
) -> anyhow::Result<()> {
    loop {
        // Draw TUI
        terminal.draw(|f| draw_ui(f, app))?;

        // Non-blocking input check
        if event::poll(Duration::from_millis(POLL_INTERVAL_MS))? {
            if let Event::Key(key_event) = event::read()? {
                match key_event.code {
                    KeyCode::Char('q') | KeyCode::Esc => break,
                    KeyCode::Down | KeyCode::Char('j') => app.next(),
                    KeyCode::Up   | KeyCode::Char('k') => app.prev(),
                    KeyCode::Char('e') => app.toggle_errors(),
                    KeyCode::Char('a') => {
                        if let Some(rowid) = app.selected_rowid() {
                            app.ack_selected();
                            db::ack_frame(conn, rowid)?;
                        }
                    }
                    KeyCode::Char('d') => {
                        if let Some(rowid) = app.selected_rowid() {
                            db::delete_frame(conn, rowid)?;
                            // Remove from local state too
                            if let Some(i) = app.list_state.selected() {
                                app.events.remove(i);
                                // Clamp selection after removal
                                if app.events.is_empty() {
                                    app.list_state.select(None);
                                } else {
                                    app.list_state.select(Some(i.min(app.events.len() - 1)));
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
        }

        // Poll DB for frames newer than last seen rowid
        let new_frames = poll_new_frames(conn, *last_rowid)?;

        for frame in new_frames {
            *last_rowid = frame.rowid;

            match decrypt_ler(&frame.raw_blob, frame.event_type, key) {
                Ok(ler) => app.push_event(frame.rowid, frame.node_id.clone(), frame.received_at, frame.rssi, ler),
                Err(e) => app.push_error(format!("row {}: decrypt failed — {e}", frame.rowid)),
            }
        }
    }

    Ok(())
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── parse_psk ────────────────────────────────────────────────────────────

    #[test]
    fn test_parse_psk_valid() {
        let hex = "a".repeat(64); // 32 bytes of 0xAA
        let key = parse_psk(&hex).unwrap();
        assert_eq!(key.len(), 32);
        assert!(key.iter().all(|&b| b == 0xaa));
    }

    #[test]
    fn test_parse_psk_trims_whitespace() {
        let hex = format!("  {}  \n", "ab".repeat(32));
        assert!(parse_psk(&hex).is_ok());
    }

    #[test]
    fn test_parse_psk_rejects_odd_hex() {
        // Odd number of hex chars — not valid hex
        let hex = "a".repeat(63);
        assert!(parse_psk(&hex).is_err());
    }

    #[test]
    fn test_parse_psk_rejects_wrong_length_short() {
        let hex = "ab".repeat(16); // 16 bytes, not 32
        let err = parse_psk(&hex).unwrap_err();
        assert!(err.to_string().contains("32 bytes"));
    }

    #[test]
    fn test_parse_psk_rejects_wrong_length_long() {
        let hex = "ab".repeat(64); // 64 bytes, not 32
        let err = parse_psk(&hex).unwrap_err();
        assert!(err.to_string().contains("32 bytes"));
    }

    #[test]
    fn test_parse_psk_rejects_non_hex() {
        let bad = "zz".repeat(32);
        assert!(parse_psk(&bad).is_err());
    }

    #[test]
    fn test_parse_psk_rejects_empty() {
        assert!(parse_psk("").is_err());
    }

    // ── resolve_db_path ──────────────────────────────────────────────────────

    #[test]
    fn test_resolve_db_path_default() {
        // Ensure LIMA_DB is not set for this test
        std::env::remove_var("LIMA_DB");
        let path = resolve_db_path();
        assert_eq!(path, PathBuf::from(DEFAULT_DB_PATH));
    }

    #[test]
    fn test_resolve_db_path_env_override() {
        std::env::set_var("LIMA_DB", "/tmp/test.db");
        let path = resolve_db_path();
        std::env::remove_var("LIMA_DB"); // clean up regardless
        assert_eq!(path, PathBuf::from("/tmp/test.db"));
    }
}
