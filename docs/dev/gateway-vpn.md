### setup mullvad
```bash
curl -fsSL https://repository.mullvad.net/deb/mullvad-keyring.asc | sudo gpg --dearmor -o /usr/share/keyrings/mullvad-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/mullvad-keyring.gpg] https://repository.mullvad.net/deb/stable $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/mullvad.list
sudo apt update -y && sudo apt install mullvad-vpn -y
```

# login and config (wifi wlan)
```bash
mullvad account login <your-account-number>
mullvad lan set allow    # so local SSH/Tailscale still works
mullvad relay set location us   # or wherever
mullvad connect
```

### bind to wlan0
```bash
mullvad interface set --name wlan0

# should show Mullvad exit IP, not your WAN
curl --interface wlan0 https://am.i.mullvad.net/ip

# should still show your real eth0 route for SSH
ip route get 8.8.8.8
```
### confirm tunnel
```bash
# Is that a Mullvad IP?
curl https://am.i.mullvad.net/connected

# What interface is the tunnel on?
ip route show table all | grep -E 'wlan|eth|tun|wg'

# Where is default traffic going?
ip route get 8.8.8.8

# check tailscale
ip route get 100.64.0.0
```
