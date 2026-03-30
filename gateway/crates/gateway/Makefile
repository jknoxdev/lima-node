run:
	cargo build
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/debug/gateway
	cargo run

build:
	cargo build
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/debug/gateway

release:
	cargo build --release
	sudo setcap 'cap_net_raw,cap_net_admin+eip' ../../target/release/gateway

