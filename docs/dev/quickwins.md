## Node (firmware) — unblocked:
 - RTC wakeup — replace k_work_reschedule stub with real PM_STATE_SOFT_OFF
 - Real PM states — hw_enter_light_sleep() and hw_enter_deep_sleep() stubs
 - Watchdog — re-enable and kick in FSM loop
 - Node ID — replace hardcoded DEADBEEF with bt_id_get()
 - Sequence counter — NVS persistence across reboots
 - Battery monitoring — ADC wiring
 - Button menu — config interface for sleep toggle, thresholds etc

Gateway — unblocked:

 - MQTT pipeline — rumqttc publisher
 - SQLite audit log — properly wired with sequence
 - Pushover notifications
 - SD card local storage
 - TUI polish — footer, seq column