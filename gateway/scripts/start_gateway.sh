#!/bin/bash
setcap 'cap_net_raw,cap_net_admin+eip' /home/arx/lima-node/gateway/target/release/gateway
hciconfig hci1 reset
sleep 1
hcitool -i hci1 lescan --duplicates > /dev/null 2>&1 &
sleep 1
exec /home/arx/lima-node/gateway/target/release/gateway --headless
