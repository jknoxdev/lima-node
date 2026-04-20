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

## Startup Script:
```bash
# Install the script
cp lima-monitor.sh ~/lima-monitor.sh
chmod +x ~/lima-monitor.sh

# Test it manually first
~/lima-monitor.sh

# Once happy — autostart as a user service
mkdir -p ~/.config/systemd/user/
cp lima-monitor.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable lima-monitor.service
systemctl --user start lima-monitor.service

# Make user services survive logout (headless)
sudo loginctl enable-linger $USER
```