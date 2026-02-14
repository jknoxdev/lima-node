# Threat Model: Kinetic IDS Node Telemetry

This document outlines the security considerations and mitigations for the kinetic ids node.

## 1. Adversary Model
We assume an adversary with physical proximity to the sensor node but no initial access to the internal MQTT network.

## 2. Attack Surface Analysis
| Threat | Impact | Mitigation |
| :--- | :--- | :--- |
| **Signal Jamming** | Denial of Service (DoS) | Implementation of a "Heartbeat" (Watchdog) on the Gateway to alert on signal loss. |
| **Replay Attack** | False State Reporting | (Future Roadmap) Implementation of rolling sequence numbers or encrypted payloads. |
| **Physical Tamper** | Bypass of Motion Switch | Utilizing internal SoC temperature and voltage monitoring to detect enclosure compromise. |
| **Unauthorized Gateway Access** | Data Exfiltration | Gateway configured with local-only MQTT authentication and isolated VLAN tagging. |

## 3. Operational Resilience
The system is designed to fail-secure. In the event of a Gateway crash, the Edge node continues to attempt transmission, and the Gateway utilizes a `systemd` watchdog for auto-recovery.
