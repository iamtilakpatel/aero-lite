# AERO-LITE Firmware & Dashboard

This folder contains everything that makes AERO-LITE run in real time:

| File             | What it does                                                     | Runs on                   |
| ---------------- | ---------------------------------------------------------------- | ------------------------- |
| `firmware.ino`   | Reads 3 CO₂ sensors, cycles the state machine, serves a JSON API | ESP32-S3                  |
| `dashboard.html` | Live web UI — plots, gauges, logs, manual controls               | Your laptop/phone browser |

The dashboard is **client-side only** (plain HTML + JavaScript). It connects directly to the ESP32 over WiFi. No cloud, no server, no install.

---

## 1. Hardware You Need

### Controller

- **ESP32-S3 DevKitC-1** (N8R8 or N16R8)

### Sensors (×3)

- **Sensirion SCD41** CO₂ / Temperature / Humidity sensors  
  _Why 3?_ One at cabin inlet, one at purified outlet, one at CO₂ tank.

### I²C Multiplexer

- **TCA9548A**  
  _Why?_ All SCD41 sensors share the same factory I²C address (`0x62`). The mux lets the ESP32 switch between them on one bus.

---

## 2. Wiring

### I²C Bus (Sensors + Mux)

| Signal | ESP32 Pin | Goes to                                      |
| ------ | --------- | -------------------------------------------- |
| SDA    | GPIO 4    | TCA9548A SDA + all SCD41 SDA (bus is shared) |
| SCL    | GPIO 5    | TCA9548A SCL + all SCD41 SCL (bus is shared) |
| 3.3V   | 3V3       | TCA9548A VCC + all SCD41 VCC                 |
| GND    | GND       | TCA9548A GND + all SCD41 GND                 |

### TCA9548A Channels

| Mux Channel | Sensor Location | SCD41 I²C Addr |
| ----------- | --------------- | -------------- |
| Channel 0   | Cabin inlet     | `0x62`         |
| Channel 1   | Purified outlet | `0x62`         |
| Channel 2   | CO₂ tank        | `0x62`         |

---

## 3. Firmware Setup (`firmware.ino`)

### Step A — Install Libraries

In **Arduino IDE** or **PlatformIO**, install:

- `Sensirion I2C SCD4x` (official Sensirion driver)

Built-in libraries used (no install needed):

- `WiFi`, `Wire`, `WebServer`

### Step B — Configure WiFi

Open `firmware.ino` and edit these lines to match your network:

```cpp
#define WIFI_SSID       "YourNetworkName"
#define WIFI_PASSWORD   "YourPassword"
```

### Step C — Flash

1. Select **"ESP32S3 Dev Module"** as the board.
2. Upload the sketch.
3. Open the **Serial Monitor** at `115200` baud.

### Step D — Get the IP Address

After boot, the Serial Monitor prints something like:

```
[OK]  IP: 192.168.1.174
[INFO] Use this IP in dashboard.html
```

**Write this IP down.** You need it for the dashboard.

---

## 4. Dashboard Setup (`dashboard.html`)

### Step A — Set the API Address

Open `dashboard.html` in any text editor. Find this line near the top:

```javascript
const API_BASE = "http://192.168.1.174";
```

Replace `192.168.1.174` with the IP address from the Serial Monitor.

### Step B — Open in Browser

Double-click `dashboard.html` or drag it into any modern browser (Chrome, Safari, etc.).

### What You'll See

- **3 sensor cards** — Cabin, Purified, CO₂ Tank
- **Live line chart** — CO₂ trace over the last ~100 seconds
- **System state pill** — ADSORB (green), REGEN (amber), PURGE (cyan)
- **Live diagnostics log** — State changes and sensor status
- **Manual controls** — Buttons to force ADSORB / REGEN / PURGE for testing

---

## 5. How It Works Together

```
┌─────────────┐      WiFi (HTTP)      ┌─────────────┐
│  dashboard  │  ←──────────────────→  │   ESP32-S3  │
│  (browser)  │   polls JSON API every  │  (firmware) │
│             │        1 second         │             │
└─────────────┘                        └─────────────┘
                                            │
                                    ┌───────┴───────┐
                                    │  TCA9548A Mux │
                                    │ Ch0 ── Cabin  │
                                    │ Ch1 ── Purified│
                                    │ Ch2 ── Tank   │
                                    └───────────────┘
```

1. The ESP32 boots, joins WiFi, and starts a web server on port 80.
2. The dashboard opens and begins polling `http://<ESP32_IP>/api/telemetry` every second.
3. The ESP32 reads all 3 sensors via the mux, updates the state machine, and replies with JSON.
4. The dashboard draws gauges, charts, and logs in real time.

---

## 6. JSON API Reference

| Endpoint         | Method | What it returns                                       |
| ---------------- | ------ | ----------------------------------------------------- |
| `/api/telemetry` | GET    | Current CO₂, humidity, temp, state, sensor health     |
| `/api/history`   | GET    | Last 100 seconds of chart data (3 CO₂ traces + state) |
| `/api/logs`      | GET    | Last 20 system events (state changes, init results)   |
| `/api/setstate`  | POST   | Manual override: `state=ADSORB`, `REGEN`, or `PURGE`  |

Example telemetry response:

```json
{
  "co2_1": 1029,
  "co2_2": 477,
  "co2_3": 633,
  "rh_1": 39.3,
  "rh_2": 15.0,
  "temp_f": 74.2,
  "state": "ADSORB",
  "state_sec": 142,
  "ok_1": true,
  "ok_2": true,
  "ok_3": true,
  "age_1": 0.5,
  "age_2": 0.5,
  "age_3": 0.5
}
```

---

## 7. First Run Checklist

| Check            | How to verify                                                   |
| ---------------- | --------------------------------------------------------------- |
| Sensors detected | Serial Monitor shows `[OK] Ch0 Cabin running` for all 3         |
| WiFi connected   | Serial prints `[OK] IP: xxx.xxx.xxx.xxx`                        |
| Dashboard loads  | Open `dashboard.html` — top-right shows ● Online                |
| Data flowing     | CO₂ numbers update every 1–5 seconds                            |
| State cycling    | Watch the pill change from ADSORB → REGEN → PURGE automatically |
| Manual override  | Click **REGEN** button — state changes immediately              |

---

## 8. Troubleshooting

| Symptom                         | Likely Cause                               | Fix                                                                  |
| ------------------------------- | ------------------------------------------ | -------------------------------------------------------------------- |
| `TCA9548A not found`            | Wrong I²C address or wiring                | Check SDA/SCL/GND/3V3. Confirm TCA address is `0x70`.                |
| `ChX not found`                 | Sensor not wired to correct mux channel    | Verify which sensor is plugged into Channel 0, 1, or 2.              |
| Dashboard shows **ERR** for CO₂ | Sensor offline or no fresh data            | Check Serial Monitor for I²C errors. Power cycle the ESP32.          |
| Dashboard dot is 🔴 red         | Sensor offline for >10 seconds             | Check wiring. Sensor may be stuck in init or I²C bus is locked.      |
| Dashboard dot is 🟠 orange      | Sensor data is 5–10 seconds old            | SCD41 updates every ~5 seconds. If orange persists, check I²C speed. |
| Dashboard shows ● Offline       | Wrong IP in `dashboard.html` or WiFi issue | Check Serial Monitor for the current IP. Update `API_BASE`.          |
| State not changing              | Timers are long (5 min / 1 min / 15 sec)   | Wait, or use manual buttons to test.                                 |

### Dashboard Status Dot Guide

The colored dot next to each sensor name tells you data freshness at a glance:

| Dot | Color      | Meaning                       | Action Needed                         |
| --- | ---------- | ----------------------------- | ------------------------------------- |
| 🟢  | **Green**  | Fresh data (< 5 seconds old)  | None — working normally               |
| 🟠  | **Orange** | Stale data (5–10 seconds old) | Monitor; may recover on next poll     |
| 🔴  | **Red**    | Offline or >10 seconds stale  | Check wiring, power cycle, verify I²C |

> **Note:** When a sensor goes red, the dashboard replaces the CO₂ number with **"ERR"** rather than showing stale data. This prevents you from acting on old readings.
