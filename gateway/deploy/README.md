# LIMA Gateway — Service Install

## Build

```bash
cd gateway
cargo build --release
```

## Install

```bash
sudo cp gateway/deploy/lima-gateway.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable lima-gateway
sudo systemctl start lima-gateway
```

## Verify

```bash
sudo systemctl status lima-gateway
journalctl -u lima-gateway -f
```

## Notes

- Mosquitto must be running: `sudo systemctl enable mosquitto`
- TUI mode still available for debugging: `./target/release/gateway`
- Headless mode: `./target/release/gateway --headless`