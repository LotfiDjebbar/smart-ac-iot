# Smart Air Conditioner — ESP32 + DHT + MQTT + Node-RED

**Author:** Djebbar Lotfi
**Simulation:** Wokwi for VS Code (PlatformIO firmware)
**Dashboard:** Node-RED (`node-red-dashboard`)

---

## 1. Objective and architecture

The goal is to build a connected air conditioner. An ESP32 measures the room temperature and humidity with a DHT sensor and drives a relay that stands for the AC compressor. It reports its state over MQTT, and a Node-RED dashboard shows the measurements and lets the user control the unit remotely. The unit runs either as an automatic thermostat or in manual mode.

```
 ┌──────────────────────────┐   telemetry (JSON, 2 s)   ┌──────────────────────┐   subscribe   ┌──────────────────────────┐
 │  ESP32 (Wokwi)           │ ────────────────────────▶ │  MQTT broker         │ ────────────▶ │  Node-RED dashboard      │
 │  DHT22 · Relay · LCD     │   status (online/offline) │  broker.hivemq.com   │               │  gauges · chart · alerts │
 │                          │ ◀──────────────────────── │  :1883               │ ◀──────────── │  switches · slider       │
 └──────────────────────────┘   cmd/mode, power, sp     └──────────────────────┘   publish     └──────────────────────────┘
        ▲ WiFi "Wokwi-GUEST" → Wokwi internet gateway                                   http://localhost:1880/ui
```

---

## 2. Components and wiring

| Component | Role | Pin | ESP32 connection |
|---|---|---|---|
| ESP32 DevKit-C v4 | Microcontroller (WiFi + MQTT) | — | — |
| DHT22 (DHT11 on real hardware) | Temperature / humidity | VCC / GND / SDA | 3V3 / GND / **GPIO15** |
| Relay module | AC compressor | VCC / GND / IN | 5V / GND / **GPIO26** |
| Relay contacts | Switch the "AC" LED | COM / NO | 3V3 / blue LED anode |
| Blue LED "AC" + 220 Ω | Shows the compressor state | Cathode → 220 Ω | GND |
| LCD 16x2 I2C (0x27) | Local display | SDA / SCL / VCC / GND | **GPIO21** / **GPIO22** / 5V / GND |
| On-board LED | MQTT connected indicator | — | **GPIO2** |

The blue LED is powered *through the relay contact* (COM → NO), so it lights only when the relay actually closes, just like a real compressor.

LCD layout:
```
T:27.5C H: 55%
AUT SP:24 AC:ON
```

---

## 3. MQTT topics

Base topic: `enp_iot/lotfi/ac`

| Topic | Direction | Payload | QoS | Retained |
|---|---|---|---|---|
| `…/telemetry` | ESP32 → Node-RED | `{"temperature":27.5,"humidity":55.0,"ac":"ON","mode":"AUTO","setpoint":24.0}` | 0 | no |
| `…/status` | ESP32 → Node-RED | `online` on connect, `offline` as **Last Will** | 1 (LWT) | yes |
| `…/cmd/mode` | Node-RED → ESP32 | `AUTO` \| `MANUAL` | 1 | no |
| `…/cmd/power` | Node-RED → ESP32 | `ON` \| `OFF` (switches to MANUAL automatically) | 1 | no |
| `…/cmd/setpoint` | Node-RED → ESP32 | number, accepted only between 16 and 30 | 1 | no |

Telemetry is published every 2 s and also immediately after every accepted command, so the dashboard updates without waiting.

---

## 4. Control logic

### AUTO mode (default): thermostat with hysteresis
With setpoint **SP = 24 °C** and hysteresis **H = 0.5 °C**:

| Temperature | Action |
|---|---|
| T > SP + 0.5 (24.5 °C) | AC **ON** |
| T < SP − 0.5 (23.5 °C) | AC **OFF** |
| 23.5 ≤ T ≤ 24.5 | **keep the current state** (dead band) |

**Why hysteresis?** A thermostat with a single threshold (ON if T > 24, OFF otherwise) switches the relay on and off over and over whenever the temperature hovers around 24 °C, because of sensor noise and small fluctuations. This *relay chattering* wears out the relay contacts and the compressor, which should not be restarted every few seconds. With a 1 °C dead band, the temperature must actually change by a full degree before the state changes again.

### MANUAL mode
The relay simply follows the last `ON` / `OFF` command from the dashboard. Sending a power command automatically switches the device to MANUAL. Switching back to AUTO hands control back to the thermostat.

### Robustness
- No `delay()` in `loop()`. Everything is timed with `millis()`, so MQTT stays responsive.
- NaN readings from the DHT are skipped and logged.
- Non-blocking reconnects: MQTT is retried every 5 s and WiFi every 10 s.
- The unique MQTT client ID comes from the chip MAC (`ESP.getEfuseMac()`).
- With the Last Will, the dashboard shows "Offline" when the ESP32 disappears without saying goodbye.
- The dashboard widgets take their state from telemetry with `passthru: false`, so they always show the **real device state** and never loop commands back.

---

## 5. How to run

### Firmware and simulation
1. In VS Code, install the **PlatformIO IDE** and **Wokwi Simulator** extensions. Wokwi asks for a free license the first time: run `Wokwi: Request a New License`.
2. Open the `smart-ac-iot` folder itself (File → Open Folder).
3. Build the firmware: `pio run`, or the ✓ *Build* button in the PlatformIO status bar.
4. Press `F1` → **Wokwi: Start Simulator**. Open `diagram.json` to see the circuit.
5. The serial monitor shows WiFi, MQTT, `[DHT]`, `[MQTT] PUB` and `[MQTT] RECV` logs.

### Dashboard
1. `npm i -g node-red`
2. `node-red`
3. Open http://localhost:1880, then go to ☰ → **Manage palette** → *Install* → `node-red-dashboard`.
4. ☰ → **Import** → select `node-red/flows.json` → *Import*.
5. Click **Deploy**.
6. Open **http://localhost:1880/ui** in Chrome or Edge. The VS Code built-in browser pauses whenever a VS Code notification appears.

### Dashboard layout
The dashboard uses a dark theme. The theme comes from the `ui_base` node, and a global CSS template styles the cards.

| Area | Content |
|---|---|
| Header | Project title, *Device online/offline* badge (from the Last Will), time of the last telemetry |
| KPI tiles | Compressor (animated fan when cooling), operating mode, setpoint with its hysteresis band, room temperature/humidity and the difference from the setpoint |
| Live measurements | Temperature gauge (0–50 °C, blue/green/red at 22/30 °C) and humidity wave gauge |
| Air conditioner control | *Automatic mode* switch, *AC power (manual)* switch, *Setpoint* slider (16–30 °C, step 0.5, sent on release) |
| Activity | The last 12 events: commands sent, compressor/mode/setpoint changes, device online/offline, alerts |
| History | Temperature and humidity line chart over the last 10 minutes |
| Alert | Red toast when the temperature reaches 35 °C |

---

## 6. Test scenario (demo)

| # | Action | Expected result |
|---|---|---|
| 1 | Start the simulation (DHT at 28 °C, SP 24) | LCD shows `Connecting...` then the values. On-board LED lights (MQTT OK). Relay closes, **blue LED ON**. Dashboard shows `🟢 Online` and `🟢 ON (cooling)` |
| 2 | Click the DHT22 in Wokwi and move the temperature to **22 °C** | Below 23.5 → relay opens, LED off, dashboard `⚪ OFF` |
| 3 | Move it to **24.2 °C** | Inside the dead band → **no change** (shows the hysteresis) |
| 4 | Move it to **25 °C** | Above 24.5 → AC ON |
| 5 | Move it to **36 °C** | Red toast alert "High temperature" on the dashboard |
| 6 | Toggle **AC power (manual)** OFF | Device goes to MANUAL (the "Automatic mode" switch turns off by itself), AC OFF although it is hot. LCD `MAN` |
| 7 | Toggle power ON / OFF | Relay and LED follow, serial shows `[MQTT] RECV` |
| 8 | Turn **Automatic mode** back on | The thermostat takes over again |
| 9 | Move the **Setpoint** slider to 30 and release | `SP:30` on the LCD. With T = 28 °C the AC turns OFF |
| 10 | Stop the simulator | After the keep-alive timeout (~15–20 s) the dashboard shows `🔴 Offline` (Last Will) |

The **History** chart shows temperature and humidity over the last 10 minutes.

---

## 7. Notes, limits and improvements

**DHT11 vs DHT22.** The assignment uses a DHT11, but Wokwi only has a DHT22 part, so the simulation uses `wokwi-dht22` with `#define DHTTYPE DHT22`. On real DHT11 hardware, change the line in `src/main.cpp` to `#define DHTTYPE DHT11`. Nothing else changes. The DHT11 is less precise (±2 °C, whole degrees, 0–50 °C), while the DHT22 gives ±0.5 °C with 0.1 °C resolution.

**Limits**
- The public broker `broker.hivemq.com` is shared with everyone. Anyone who knows the topic can read the data or send commands.
- There is no TLS and no authentication: traffic is in clear text on port 1883.
- The relay only simulates the compressor. A real split AC is usually driven by infrared.

**Possible improvements**
- A private broker (Mosquitto) with username/password, TLS on port 8883, and ACLs per topic.
- Storage of the history in **InfluxDB**, with long-term visualisation in **Grafana**.
- Real control of an AC unit with an **IR LED** (IRremoteESP8266 library) instead of a relay.
- A setpoint saved in flash (Preferences/NVS) so it survives a reboot, and schedules (night mode).
