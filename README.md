# Base Station Firmware

ESP-IDF firmware for the robot base station.

## Role

- hosts the local setup/control dashboard
- manages Wi-Fi provisioning and backend connectivity
- relays commands to the robot over LoRa
- receives telemetry/acks from the gateway side
- synchronizes robot/base-station state with the backend service

## Main Files

- `main/salt_base_station.c` - primary firmware logic, HTTP server, LoRa flow, backend sync
- `components` - display, radio, and supporting ESP-IDF components

## Build

From an ESP-IDF shell:

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Notes

- The working tree may contain tracked `build/` artifacts after local builds; source changes of interest normally live under `main/` and `components/`.
- The status/dashboard flow is built around the base station being the bridge between the cloud backend and the field radio link.
