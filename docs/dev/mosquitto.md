### setup on rpi5

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto```

### test

```bash
systemctl status mosquitto
# test it works:
mosquitto_sub -t 'lima/#' -v &
mosquitto_pub -t 'lima/test' -m 'hello'
```