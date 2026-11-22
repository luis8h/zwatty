#pragma once
// Build-time configuration. Edit, then re-flash with `pio run -t upload`.

// BLE name shown in your app's device scan.
#define BLE_DEVICE_NAME "FTMS-SIM"

// Wi-Fi access point for the web UI (http://192.168.4.1).
// Password must be at least 8 characters; shorter/empty = open network.
#define WIFI_AP_SSID     "FTMS-SIM"
#define WIFI_AP_PASSWORD "simtrainer"
#define WIFI_AP_CHANNEL  6

#define SERIAL_BAUD 115200

// Limits advertised over FTMS (and enforced by the control point).
#define POWER_RANGE_MIN  0
#define POWER_RANGE_MAX  1800   // W   (KICKR Core: 0-1800)
#define RESISTANCE_MAX   100    // 0.1 units, i.e. 0.0 - 10.0
