use btleplug::api::{Manager as _, Central, Peripheral, ScanFilter};
use btleplug::platform::Manager;
use std::error::Error;
use tokio::time::{sleep, Duration};

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let manager = Manager::new().await?;
    let adapters = manager.adapters().await?;
    let adapter = adapters.into_iter().next()
        .expect("No BLE adapter found");

    println!("Starting LIMA gateway scanner...");
    adapter.start_scan(ScanFilter::default()).await?;

    loop {
        let peripherals = adapter.peripherals().await?;
        for p in peripherals {
            if let Some(props) = p.properties().await? {
                if props.local_name.as_deref().map(|n| n.starts_with("LIMA")).unwrap_or(false) {
                    for (_, bytes) in &props.manufacturer_data {
                        if let Some(decoded) = decode_lima_payload(bytes) {
                            println!("LIMA | RSSI: {:?} | {}", props.rssi, decoded);
                        }
                    }
                }
            }
        }
        sleep(Duration::from_millis(100)).await;
    }
}

fn decode_lima_payload(data: &[u8]) -> Option<String> {
    if data.len() < 24 {
        return None;
    }

    // node_id: bytes 0-5
    let node_id = &data[0..6];
    // event_type: byte 6
    let event_type = data[6];
    // reserved: byte 7
    // sequence: bytes 8-11 (u32 little-endian)
    let sequence = u32::from_le_bytes(data[8..12].try_into().ok()?);
    // timestamp_ms: bytes 12-15 (u32 little-endian)
    let timestamp_ms = u32::from_le_bytes(data[12..16].try_into().ok()?);
    // accel_g: bytes 16-19 (f32 little-endian)
    let accel_g = f32::from_le_bytes(data[16..20].try_into().ok()?);
    // delta_pa: bytes 20-23 (f32 little-endian)
    let delta_pa = f32::from_le_bytes(data[20..24].try_into().ok()?);

    Some(format!(
        "node={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} evt=0x{:02X} seq={} t={}ms accel={:.2}g baro={:.2}Pa",
        node_id[0], node_id[1], node_id[2], node_id[3], node_id[4], node_id[5],
        event_type, sequence, timestamp_ms, accel_g, delta_pa
    ))
}