use ratatui::{
    layout::{Constraint, Direction, Layout},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, List, ListItem, ListState, Paragraph},
    Frame,
};

use crate::crypto::Ler;

// ── Event entry ───────────────────────────────────────────────────────────────

/// A decrypted event as displayed in the TUI.
#[allow(dead_code)]
#[derive(Debug, Clone)]
pub struct EventEntry {
    pub rowid:       i64,
    pub node_id: String,
    pub ler:         Ler,
    #[allow(dead_code)]
    pub received_at: i64,   // Unix timestamp from DB
    pub acked:       bool,
    pub rssi:    i32,
}

/// A display-only error entry (failed decrypt, hex decode, etc.)
#[derive(Debug, Clone)]
pub struct ErrorEntry {
    #[allow(dead_code)]
    pub message: String,
}

// ── AppState ──────────────────────────────────────────────────────────────────

/// All mutable state for the TUI. Pure data — no ratatui types here.
/// Keeps rendering logic and state logic fully separated.
pub struct AppState {
    pub events:       Vec<EventEntry>,
    pub errors:       Vec<ErrorEntry>,
    pub list_state:   ListState,       // ratatui scroll/selection cursor
    pub show_errors:  bool,            // toggle error panel
}

impl AppState {
    pub fn new() -> Self {
        Self {
            events:      Vec::new(),
            errors:      Vec::new(),
            list_state:  ListState::default(),
            show_errors: false,
        }
    }

    /// Push a newly decrypted LER into the event list.
    pub fn push_event(&mut self, rowid: i64, node_id: String, received_at: i64, rssi: i32, ler: Ler) {
        self.events.push(EventEntry { rowid, node_id, ler, received_at, rssi, acked: false });
        // Auto-select the latest event if nothing is selected
        if self.list_state.selected().is_none() {
            self.list_state.select(Some(0));
        }
    }

    /// Push a non-fatal error message (failed decrypt, corrupt blob, etc.)
    pub fn push_error(&mut self, message: String) {
        self.errors.push(ErrorEntry { message });
    }

    /// Move selection down one row.
    pub fn next(&mut self) {
        if self.events.is_empty() {
            return;
        }
        let i = match self.list_state.selected() {
            Some(i) => (i + 1).min(self.events.len() - 1),
            None    => 0,
        };
        self.list_state.select(Some(i));
    }

    /// Move selection up one row.
    pub fn prev(&mut self) {
        if self.events.is_empty() {
            return;
        }
        let i = match self.list_state.selected() {
            Some(i) => i.saturating_sub(1),
            None    => 0,
        };
        self.list_state.select(Some(i));
    }

    /// Mark the currently selected event as acked in local state.
    /// Caller is responsible for writing acked_at to DB via db::ack_frame().
    pub fn ack_selected(&mut self) {
        if let Some(i) = self.list_state.selected() {
            if let Some(entry) = self.events.get_mut(i) {
                entry.acked = true;
            }
        }
    }

    /// Return the rowid of the currently selected event, if any.
    pub fn selected_rowid(&self) -> Option<i64> {
        self.list_state
            .selected()
            .and_then(|i| self.events.get(i))
            .map(|e| e.rowid)
    }

    /// Toggle the error panel visibility.
    pub fn toggle_errors(&mut self) {
        self.show_errors = !self.show_errors;
    }

    /// Unacked event count — shown in the header.
    pub fn unacked_count(&self) -> usize {
        self.events.iter().filter(|e| !e.acked).count()
    }
}

// ── Rendering ─────────────────────────────────────────────────────────────────

/// Top-level draw call — called on every tick from run_loop.
pub fn draw_ui(f: &mut Frame, app: &mut AppState) {
    // Layout: header / event list / detail panel / footer
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),  // header
            Constraint::Min(5),     // event list
            Constraint::Length(6),  // detail panel
            Constraint::Length(3),  // footer / keybindings
        ])
        .split(f.area());

    draw_header(f, app, chunks[0]);
    draw_event_list(f, app, chunks[1]);
    draw_detail(f, app, chunks[2]);
    draw_footer(f, chunks[3]);
}

fn draw_header(f: &mut Frame, app: &AppState, area: ratatui::layout::Rect) {
    let unacked = app.unacked_count();
    let errors  = app.errors.len();

    let title = Line::from(vec![
        Span::styled(" L.I.M.A. ", Style::default()
            .fg(Color::Cyan)
            .add_modifier(Modifier::BOLD)),
        Span::raw("│ "),
        Span::styled(
            format!("{} unacked", unacked),
            if unacked > 0 {
                Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
            } else {
                Style::default().fg(Color::Green)
            },
        ),
        Span::raw("  "),
        Span::styled(
            format!("{} errors", errors),
            if errors > 0 {
                Style::default().fg(Color::Red)
            } else {
                Style::default().fg(Color::DarkGray)
            },
        ),
    ]);

    let block = Paragraph::new(title)
        .block(Block::default().borders(Borders::ALL));
    f.render_widget(block, area);
}

fn draw_event_list(f: &mut Frame, app: &mut AppState, area: ratatui::layout::Rect) {
    let items: Vec<ListItem> = app.events.iter().map(|entry| {
        let node_hex = entry.ler.node_id
            .iter()
            .map(|b| format!("{:02x}", b))
            .collect::<Vec<_>>()
            .join(":");

        let ack_marker = if entry.acked { "✓" } else { "●" };
        let ack_color  = if entry.acked { Color::DarkGray } else { Color::Yellow };

        let line = Line::from(vec![
            Span::styled(format!(" {} ", ack_marker), Style::default().fg(ack_color)),
            Span::styled(format!("{:<6} ", entry.rowid), Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} ", node_hex), Style::default().fg(Color::Cyan)),
            Span::styled(
                format!("evt={:#04x} ", entry.ler.event_type),
                Style::default().fg(Color::Magenta),
            ),
            Span::styled(
                format!("seq={:<6} ", entry.ler.sequence),
                Style::default().fg(Color::White),
            ),
            Span::styled(
                format!("accel={:.3}g ", entry.ler.accel_g),
                Style::default().fg(Color::Green),
            ),
            Span::styled(
                format!("ΔPa={:.2}", entry.ler.delta_pa),
                Style::default().fg(Color::Blue),
            ),
        ]);

        ListItem::new(line)
    }).collect();

    let list = List::new(items)
        .block(Block::default()
            .borders(Borders::ALL)
            .title(" Events "))
        .highlight_style(Style::default()
            .bg(Color::DarkGray)
            .add_modifier(Modifier::BOLD))
        .highlight_symbol("▶ ");

    f.render_stateful_widget(list, area, &mut app.list_state);
}

fn draw_detail(f: &mut Frame, app: &AppState, area: ratatui::layout::Rect) {
    let content = match app.list_state.selected().and_then(|i| app.events.get(i)) {
        None => vec![Line::from(Span::styled(
            " No event selected",
            Style::default().fg(Color::DarkGray),
        ))],
        Some(entry) => {
            let node_hex = entry.ler.node_id
                .iter()
                .map(|b| format!("{:02x}", b))
                .collect::<Vec<_>>()
                .join(":");

            vec![
                Line::from(vec![
                    Span::styled(" node      ", Style::default().fg(Color::DarkGray)),
                    Span::styled(node_hex, Style::default().fg(Color::Cyan)),
                ]),
                Line::from(vec![
                    Span::styled(" event     ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        format!("{:#04x}", entry.ler.event_type),
                        Style::default().fg(Color::Magenta),
                    ),
                ]),
                Line::from(vec![
                    Span::styled(" sequence  ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        entry.ler.sequence.to_string(),
                        Style::default().fg(Color::White),
                    ),
                ]),
                Line::from(vec![
                    Span::styled(" timestamp ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        format!("{}ms", entry.ler.timestamp_ms),
                        Style::default().fg(Color::White),
                    ),
                ]),
                Line::from(vec![
                    Span::styled(" accel     ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        format!("{:.4} g", entry.ler.accel_g),
                        Style::default().fg(Color::Green),
                    ),
                ]),
                Line::from(vec![
                    Span::styled(" delta_pa  ", Style::default().fg(Color::DarkGray)),
                    Span::styled(
                        format!("{:.4} Pa", entry.ler.delta_pa),
                        Style::default().fg(Color::Blue),
                    ),
                ]),
            ]
        }
    };

    let block = Paragraph::new(content)
        .block(Block::default()
            .borders(Borders::ALL)
            .title(" Detail "));
    f.render_widget(block, area);
}

fn draw_footer(f: &mut Frame, area: ratatui::layout::Rect) {
    let keys = Line::from(vec![
        key_hint("↑↓", "scroll"),
        Span::raw("  "),
        key_hint("a", "ack"),
        Span::raw("  "),
        key_hint("d", "delete"),
        Span::raw("  "),
        key_hint("e", "toggle errors"),
        Span::raw("  "),
        key_hint("q", "quit"),
    ]);

    let block = Paragraph::new(keys)
        .block(Block::default().borders(Borders::ALL));
    f.render_widget(block, area);
}

fn key_hint(key: &str, label: &str) -> Span<'static> {
    Span::styled(
        format!("[{}] {}", key, label),
        Style::default().fg(Color::DarkGray),
    )
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn test_ler(seq: u32) -> Ler {
        Ler {
            node_id:      [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF],
            event_type:   0x01,
            sequence:     seq,
            timestamp_ms: 1_700_000_000,
            accel_g:      1.0,
            delta_pa:     0.0,
        }
    }

    fn push_n(app: &mut AppState, n: u32) {
        for i in 0..n {
            app.push_event(i as i64, "aa:bb:cc:dd:ee:ff".to_string(), 1000 + i as i64, -70, test_ler(i));
        }
    }

    // ── push / counts ─────────────────────────────────────────────────────────

    #[test]
    fn test_push_event_appends() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        assert_eq!(app.events.len(), 3);
    }

    #[test]
    fn test_push_event_auto_selects_first() {
        let mut app = AppState::new();
        app.push_event(1, "aa:bb:cc:dd:ee:ff".to_string(), 1000, -70, test_ler(0));
        assert_eq!(app.list_state.selected(), Some(0));
    }

    #[test]
    fn test_push_event_does_not_move_selection_on_subsequent() {
        let mut app = AppState::new();
        push_n(&mut app, 1);
        app.next(); // move to 0 (already there, but explicit)
        push_n(&mut app, 2);
        // selection should stay where it was
        assert_eq!(app.list_state.selected(), Some(0));
    }

    #[test]
    fn test_push_error_appends() {
        let mut app = AppState::new();
        app.push_error("something broke".into());
        assert_eq!(app.errors.len(), 1);
        assert_eq!(app.errors[0].message, "something broke");
    }

    #[test]
    fn test_unacked_count() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        assert_eq!(app.unacked_count(), 3);
    }

    // ── navigation ────────────────────────────────────────────────────────────

    #[test]
    fn test_next_advances_selection() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(0));
        app.next();
        assert_eq!(app.list_state.selected(), Some(1));
    }

    #[test]
    fn test_next_clamps_at_end() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(2));
        app.next();
        assert_eq!(app.list_state.selected(), Some(2));
    }

    #[test]
    fn test_prev_moves_back() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(2));
        app.prev();
        assert_eq!(app.list_state.selected(), Some(1));
    }

    #[test]
    fn test_prev_clamps_at_zero() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(0));
        app.prev();
        assert_eq!(app.list_state.selected(), Some(0));
    }

    #[test]
    fn test_next_on_empty_does_not_panic() {
        let mut app = AppState::new();
        app.next(); // should be a no-op
        assert_eq!(app.list_state.selected(), None);
    }

    #[test]
    fn test_prev_on_empty_does_not_panic() {
        let mut app = AppState::new();
        app.prev();
        assert_eq!(app.list_state.selected(), None);
    }

    // ── ack ───────────────────────────────────────────────────────────────────

    #[test]
    fn test_ack_selected_marks_entry() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(1));
        app.ack_selected();
        assert!(app.events[1].acked);
        assert!(!app.events[0].acked);
        assert!(!app.events[2].acked);
    }

    #[test]
    fn test_ack_reduces_unacked_count() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(Some(0));
        app.ack_selected();
        assert_eq!(app.unacked_count(), 2);
    }

    #[test]
    fn test_ack_on_nothing_selected_does_not_panic() {
        let mut app = AppState::new();
        push_n(&mut app, 3);
        app.list_state.select(None);
        app.ack_selected(); // no-op
        assert_eq!(app.unacked_count(), 3);
    }

    // ── selected_rowid ────────────────────────────────────────────────────────

    #[test]
    fn test_selected_rowid_returns_correct_rowid() {
        let mut app = AppState::new();
        app.push_event(42, "aa:bb:cc:dd:ee:ff".to_string(), 1000, -70, test_ler(0));
        app.push_event(99, "aa:bb:cc:dd:ee:ff".to_string(), 1001, -68, test_ler(1));
        app.list_state.select(Some(1));
        assert_eq!(app.selected_rowid(), Some(99));
    }

    #[test]
    fn test_selected_rowid_none_when_empty() {
        let app = AppState::new();
        assert_eq!(app.selected_rowid(), None);
    }

    // ── toggle errors ─────────────────────────────────────────────────────────

    #[test]
    fn test_toggle_errors() {
        let mut app = AppState::new();
        assert!(!app.show_errors);
        app.toggle_errors();
        assert!(app.show_errors);
        app.toggle_errors();
        assert!(!app.show_errors);
    }
}
