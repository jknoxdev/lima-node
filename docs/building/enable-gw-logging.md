journal persistence:
```bash
sudomkdir -p /var/log/journal
sudo sed -i 's/^#\?Storage=.*/Storage=persistent/' /etc/systemd/journald.conf

# add a cap so it can't eat the SD card:
echo'SystemMaxUse=500M'|sudotee -a /etc/systemd/journald.conf
sudo systemctl restart systemd-journald
```


per-service:
```bash
journalctl -u lima-gateway -f  # live follow
journalctl -u lima-gateway -b  # this boot only
journalctl -u lima-gateway -u lima-lescan -u mosquitto --since "15 min ago"
```

