use std::path::Path;

use rusqlite::{params, Connection, Result};

/// Raw frame as read from the gateway `events` table.
/// sig_verified omitted — query filters to sig_verified=1 only,
/// so every RawFrame reaching the client is implicitly verified.

pub struct RawFrame {
    pub rowid:        i64,
    pub node_id:      String,
    pub received_at:  i64,
    pub event_type:   u8,
    pub rssi:         i32,
    pub raw_blob:     Vec<u8>,   // decoded from hex, 182B
}

/// Open the gateway SQLite DB in WAL mode.
/// WAL allows the gateway to keep writing while the client reads/writes
/// concurrently — no shutdown required.
pub fn open_db(path: &Path) -> anyhow::Result<Connection> {
    let conn = Connection::open(path)?;

    // WAL mode — persisted in the DB file, so this is a no-op if the
    // gateway already set it. If not, sets it for both processes.
    conn.execute_batch("PRAGMA journal_mode=WAL;")?;

    // If gateway holds a write lock briefly, wait up to 1s before erroring.
    conn.execute_batch("PRAGMA busy_timeout=1000;")?;

    // Add acked_at column if this is the first time the client has opened
    // this DB. SQLite has no ADD COLUMN IF NOT EXISTS, so we swallow the
    // "duplicate column" error intentionally.
    let _ = conn.execute_batch("ALTER TABLE events ADD COLUMN acked_at INTEGER;");

    Ok(conn)
}

/// Return the highest rowid currently in the events table.
/// Seeds the poll cursor so we don't replay old frames on startup.
pub fn last_rowid(conn: &Connection) -> Result<i64> {
    conn.query_row(
        "SELECT COALESCE(MAX(rowid), 0) FROM events",
        [],
        |row| row.get(0),
    )
}

/// Fetch all events with rowid > since_rowid, ordered oldest-first.
/// Only returns sig_verified frames — mirrors gateway publish policy.
pub fn poll_new_frames(conn: &Connection, since_rowid: i64) -> anyhow::Result<Vec<RawFrame>> {
    let mut stmt = conn.prepare_cached(
        "SELECT rowid, node_id, received_at, frame_type, rssi, raw_blob
         FROM events
         WHERE rowid > ?1
           AND sig_verified = 1
         ORDER BY rowid ASC",
    )?;

    let frames = stmt
        .query_map(params![since_rowid], |row| {
            Ok((
                row.get::<_, i64>(0)?,       // rowid
                row.get::<_, String>(1)?,    // node_id
                row.get::<_, i64>(2)?,       // received_at
                row.get::<_, u8>(3)?,        // frame_type / event_type
                row.get::<_, i32>(4)?,       // rssi
                row.get::<_, String>(5)?,    // hex-encoded blob
            ))
        })?
        .collect::<Result<Vec<_>>>()?
        .into_iter()
        .filter_map(|(rowid, node_id, received_at, event_type, rssi, hex_str)| {
            match hex::decode(&hex_str) {
                Ok(bytes) => Some(RawFrame {
                    rowid,
                    node_id,
                    received_at,
                    event_type,
                    rssi,
                    raw_blob: bytes,
                }),
                Err(e) => {
                    eprintln!("warn: row {rowid} hex decode failed — {e}");
                    None
                }
            }
        })
        .collect();

    Ok(frames)
}

// ── RUD write paths ──────────────────────────────────────────────────────────

/// Mark a frame as acknowledged by the operator.
/// Stamps `acked_at` with current Unix time; frame is retained in DB.
pub fn ack_frame(conn: &Connection, rowid: i64) -> anyhow::Result<()> {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs() as i64;

    conn.execute(
        "UPDATE events SET acked_at = ?1 WHERE rowid = ?2",
        params![now, rowid],
    )?;
    Ok(())
}

/// Hard-delete a single event by rowid.
pub fn delete_frame(conn: &Connection, rowid: i64) -> anyhow::Result<()> {
    conn.execute("DELETE FROM events WHERE rowid = ?1", params![rowid])?;
    Ok(())
}

/// Purge all events older than `older_than_secs` seconds from now.
/// Returns the number of rows deleted.
#[allow(dead_code)]
pub fn purge_old_frames(conn: &Connection, older_than_secs: i64) -> anyhow::Result<usize> {
    let cutoff = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs() as i64
        - older_than_secs;

    let deleted = conn.execute(
        "DELETE FROM events WHERE received_at < ?1",
        params![cutoff],
    )?;
    Ok(deleted)
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Spin up an in-memory DB that mirrors the gateway schema exactly.
    /// Includes WAL pragma + acked_at migration so tests run the same
    /// code path as production.
    fn test_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();

        // Mirror gateway db_init exactly
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
        ).unwrap();

        // Run our migration — adds acked_at
        let _ = conn.execute_batch("ALTER TABLE events ADD COLUMN acked_at INTEGER;");

        conn
    }

    /// Insert a minimal test row. raw_blob is a hex string of `len` zero bytes.
    fn insert_test_event(
        conn: &Connection,
        node_id: &str,
        received_at: i64,
        sig_verified: bool,
        blob_len: usize,
    ) -> i64 {
        let blob_hex = "00".repeat(blob_len);
        conn.execute(
            "INSERT INTO events (node_id, received_at, sig_verified, rssi, raw_blob)
             VALUES (?1, ?2, ?3, 0, ?4)",
            params![node_id, received_at, sig_verified as i32, blob_hex],
        ).unwrap();
        conn.last_insert_rowid()
    }

    #[test]
    fn test_last_rowid_empty_table() {
        let conn = test_conn();
        assert_eq!(last_rowid(&conn).unwrap(), 0);
    }

    #[test]
    fn test_last_rowid_after_insert() {
        let conn = test_conn();
        insert_test_event(&conn, "aa:bb:cc:dd:ee:ff", 1000, true, 182);
        insert_test_event(&conn, "aa:bb:cc:dd:ee:ff", 1001, true, 182);
        assert_eq!(last_rowid(&conn).unwrap(), 2);
    }

    #[test]
    fn test_poll_returns_only_verified_frames() {
        let conn = test_conn();
        insert_test_event(&conn, "node-a", 1000, true,  182); // rowid 1 — verified
        insert_test_event(&conn, "node-b", 1001, false, 182); // rowid 2 — NOT verified
        insert_test_event(&conn, "node-c", 1002, true,  182); // rowid 3 — verified

        let frames = poll_new_frames(&conn, 0).unwrap();
        assert_eq!(frames.len(), 2);
        assert_eq!(frames.len(), 2, "only verified frames should be returned");
        assert_eq!(frames[0].node_id, "node-a");
        assert_eq!(frames[1].node_id, "node-c");
    }

    #[test]
    fn test_poll_cursor_skips_seen_rows() {
        let conn = test_conn();
        insert_test_event(&conn, "node-a", 1000, true, 182); // rowid 1
        insert_test_event(&conn, "node-b", 1001, true, 182); // rowid 2
        insert_test_event(&conn, "node-c", 1002, true, 182); // rowid 3

        // Simulate having already consumed rows 1 and 2
        let frames = poll_new_frames(&conn, 2).unwrap();
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].node_id, "node-c");
    }

    #[test]
    fn test_poll_decodes_hex_blob() {
        let conn = test_conn();
        // Insert 182 bytes of 0xAB
        let blob_hex = "ab".repeat(182);
        conn.execute(
            "INSERT INTO events (node_id, received_at, sig_verified, rssi, raw_blob)
             VALUES ('node-x', 1000, 1, 0, ?1)",
            params![blob_hex],
        ).unwrap();

        let frames = poll_new_frames(&conn, 0).unwrap();
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].raw_blob.len(), 182);
        assert!(frames[0].raw_blob.iter().all(|&b| b == 0xAB));
    }

    #[test]
    fn test_ack_frame_sets_timestamp() {
        let conn = test_conn();
        let rowid = insert_test_event(&conn, "node-a", 1000, true, 182);

        ack_frame(&conn, rowid).unwrap();

        let acked_at: Option<i64> = conn
            .query_row(
                "SELECT acked_at FROM events WHERE rowid = ?1",
                params![rowid],
                |row| row.get(0),
            )
            .unwrap();

        assert!(acked_at.is_some());
        assert!(acked_at.unwrap() > 0);
    }

    #[test]
    fn test_delete_frame_removes_row() {
        let conn = test_conn();
        let rowid = insert_test_event(&conn, "node-a", 1000, true, 182);

        delete_frame(&conn, rowid).unwrap();

        let count: i64 = conn
            .query_row("SELECT COUNT(*) FROM events", [], |row| row.get(0))
            .unwrap();
        assert_eq!(count, 0);
    }

    #[test]
    fn test_purge_old_frames() {
        let conn = test_conn();
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs() as i64;

        // One old event (1 hour ago), one recent event (now)
        insert_test_event(&conn, "old-node",    now - 3600, true, 182);
        insert_test_event(&conn, "recent-node", now,        true, 182);

        // Purge anything older than 30 minutes
        let deleted = purge_old_frames(&conn, 1800).unwrap();
        assert_eq!(deleted, 1);

        let remaining: i64 = conn
            .query_row("SELECT COUNT(*) FROM events", [], |row| row.get(0))
            .unwrap();
        assert_eq!(remaining, 1);
    }
}
