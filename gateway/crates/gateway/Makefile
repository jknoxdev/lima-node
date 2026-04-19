run:
	sudo systemctl start mosquitto || true
	sudo hciconfig hci1 reset
	sleep 1
	# btleplug on Linux defaults to passive scan — cannot receive extended adv frames
	# hcitool lescan puts hci1 into active mode; btleplug inherits the session
	sudo hcitool -i hci1 lescan --duplicates &
	sleep 1
	cargo build
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/debug/gateway
	cargo run

build:
	cargo build
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/debug/gateway

release:
	cargo build --release
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/release/gateway

mqtt-start:
	sudo systemctl start mosquitto

mqtt-stop:
	sudo systemctl stop mosquitto

mqtt-status:
	systemctl status mosquitto

mqtt-log:
	mosquitto_sub -t 'lima/#' -v

deploy: 
	sudo cp ../../deploy/lima-gateway.service /etc/systemd/system/lima-gateway.service
	sudo systemctl daemon-reload
	sudo systemctl restart lima-gateway
	sudo systemctl status lima-gateway || true

radiomon:
	sudo hcitool -i hci1 cmd 0x08 0x000c 0x00 0x00
	sudo hcitool -i hci1 cmd 0x08 0x000b 0x01 0x00 0xa0 0x00 0x50 0x00 0x00 0x00
	sudo hcitool -i hci1 cmd 0x08 0x000c 0x01 0x01
	sudo systemctl daemon-reload
	sudo systemctl restart lima-gateway
	sudo systemctl status lima-gateway || true
	sudo journalctl -u lima-gateway -f

mon: 
	sudo systemctl daemon-reload
	sudo systemctl restart lima-gateway
	sudo systemctl status lima-gateway || true
	sudo journalctl -u lima-gateway -f