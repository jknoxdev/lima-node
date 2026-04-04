run:
	sudo systemctl start mosquitto || true
	sudo hciconfig hci0 reset
	sleep 1
	sudo hcitool -i hci0 lescan --duplicates &
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
