# client/Makefile — LIMA operator client
#
# The client never touches the radio. It reads the gateway's SQLite DB and
# decrypts LER payloads in-process. No sudo, no setcap, no adapter contention:
# run it alongside a live lima-gateway.service.

BIN     := ./target/release/client
DEBUG   := ./target/debug/client
DB      ?= $(HOME)/lima-node/gateway/crates/gateway/lima_gateway.db

.PHONY: build release run live dev check test clean db-check gw-check

release:
	cargo build --release

build:
	cargo build

# Replay all verified frames from rowid 0 (default demo mode)
run: release db-check gw-check
	LIMA_DB=$(DB) $(BIN)

# Start from current max rowid — only frames arriving after launch
live: release db-check
	LIMA_DB=$(DB) LIMA_LIVE=1 $(BIN)

# Same as run, but against the debug build
dev: build db-check
	LIMA_DB=$(DB) $(DEBUG)

check:
	cargo check
	cargo clippy -- -D warnings

test:
	cargo test

# Confirm the DB exists and has rows before entering the TUI —
# an empty table looks identical to a bad PSK once the TUI takes over.
db-check:
	@test -f $(DB) || { echo "ERROR: DB not found at $(DB)"; exit 1; }
	@echo "DB   → $(DB)"
	@sqlite3 $(DB) ".tables" | tr -s ' ' '\n' | sed '/^$$/d' | sed 's/^/     table: /'

# The client is a reader; the gateway must be up for new rows to appear.
gw-check:
	@systemctl is-active --quiet lima-gateway \
		&& echo "lima-gateway: active (PID $$(systemctl show -p MainPID --value lima-gateway))" \
		|| echo "lima-gateway: NOT RUNNING — no new frames will arrive"
	@echo "gateway processes:"; pgrep -af '[g]ateway' || echo "  none"

clean:
	cargo clean
