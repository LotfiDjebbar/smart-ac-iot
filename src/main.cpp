/*
 * =============================================================================
 *  Smart Air Conditioner - ESP32 + DHT + MQTT + Node-RED
 *  Author : Djebbar Lotfi
 *
 *  - Reads temperature / humidity from a DHT sensor every 2 s
 *  - Drives a relay (= AC compressor) in two modes:
 *        AUTO   : thermostat with 0.5 °C hysteresis around a setpoint
 *        MANUAL : relay follows ON/OFF commands from the dashboard
 *  - Publishes telemetry (JSON) over MQTT and receives commands
 *  - Shows the state on a 16x2 I2C LCD
 *
 *  Fully non-blocking: no delay() in loop(), everything is timed with millis().
 * =============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ----------------------------------------------------------------------------
//  Configuration
// ----------------------------------------------------------------------------

// WiFi (Wokwi simulated access point: open network on channel 6)
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASS = "";
const int WIFI_CHANNEL = 6;

// MQTT broker (public, reachable through the Wokwi internet gateway)
const char *MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// MQTT topics
#define TOPIC_BASE "enp_iot/lotfi/ac"
const char *TOPIC_TELEMETRY = TOPIC_BASE "/telemetry";
const char *TOPIC_STATUS = TOPIC_BASE "/status";
const char *TOPIC_CMD_MODE = TOPIC_BASE "/cmd/mode";
const char *TOPIC_CMD_POWER = TOPIC_BASE "/cmd/power";
const char *TOPIC_CMD_SETPOINT = TOPIC_BASE "/cmd/setpoint";

// Pins
#define DHT_PIN 15        // DHT data pin
#define RELAY_PIN 26      // relay IN (AC compressor)
#define MQTT_LED_PIN 2    // on-board LED = MQTT connected indicator
// LCD uses the default ESP32 I2C pins: SDA = GPIO21, SCL = GPIO22

// Sensor type.
// Wokwi only provides a DHT22 part, so the simulation uses DHT22.
// On real DHT11 hardware, change to DHT11.
#define DHTTYPE DHT22

// LCD
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// Control parameters
const float HYSTERESIS = 0.5f;        // °C, half-width of the dead band
const float SETPOINT_DEFAULT = 24.0f; // °C
const float SETPOINT_MIN = 16.0f;
const float SETPOINT_MAX = 30.0f;

// Timing (ms)
const unsigned long SENSOR_INTERVAL = 2000;    // read sensor + publish telemetry
const unsigned long MQTT_RETRY_INTERVAL = 5000; // MQTT reconnect attempts
const unsigned long WIFI_RETRY_INTERVAL = 10000; // WiFi reconnect attempts
                                                 // (longer than a normal connect)

// ----------------------------------------------------------------------------
//  Global objects and state
// ----------------------------------------------------------------------------

DHT dht(DHT_PIN, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

enum Mode { MODE_AUTO, MODE_MANUAL };

Mode mode = MODE_AUTO;            // default mode
float setpoint = SETPOINT_DEFAULT;
bool acOn = false;                // current relay state
bool manualPower = false;         // requested state in MANUAL mode

float temperature = NAN;          // last valid readings
float humidity = NAN;

char clientId[32];

unsigned long lastSensorMs = 0;
unsigned long lastMqttRetryMs = 0;
unsigned long lastWifiRetryMs = 0;
bool wifiWasConnected = false;

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

const char *modeName(Mode m) { return m == MODE_AUTO ? "AUTO" : "MANUAL"; }

// Apply a new relay state (only logs when it actually changes)
void setAc(bool on) {
  if (on != acOn) {
    acOn = on;
    digitalWrite(RELAY_PIN, acOn ? HIGH : LOW);
    Serial.printf("[AC]    Compressor %s\n", acOn ? "ON" : "OFF");
  }
}

// Core control logic, called after each new reading or command
void updateControl() {
  if (mode == MODE_MANUAL) {
    setAc(manualPower);
    return;
  }

  // AUTO mode: thermostat with hysteresis
  if (isnan(temperature)) return; // no valid reading yet: keep current state

  if (temperature > setpoint + HYSTERESIS) {
    setAc(true);
  } else if (temperature < setpoint - HYSTERESIS) {
    setAc(false);
  }
  // Inside the dead band [SP-0.5 ; SP+0.5] the relay keeps its state.
}

// Refresh the LCD (lines are padded to 16 chars to erase old characters)
void updateLcd() {
  char line1[17];
  char line2[24];

  // Line 1: "T:27.5C H: 55%"
  if (isnan(temperature) || isnan(humidity)) {
    snprintf(line1, sizeof(line1), "T:--.-C H:---%%");
  } else {
    snprintf(line1, sizeof(line1), "T:%4.1fC H:%3d%%", temperature,
             (int)roundf(humidity));
  }

  // Line 2: "AUT SP:24 AC:ON"  (setpoint shown with .5 when needed)
  char sp[8];
  if (fabsf(setpoint - roundf(setpoint)) < 0.01f) {
    snprintf(sp, sizeof(sp), "%.0f", setpoint);
  } else {
    snprintf(sp, sizeof(sp), "%.1f", setpoint);
  }
  const char *m = (mode == MODE_AUTO) ? "AUT" : "MAN";
  const char *s = acOn ? "ON" : "OFF";
  snprintf(line2, sizeof(line2), "%s SP:%s AC:%s", m, sp, s);
  if (strlen(line2) > LCD_COLS) {
    // e.g. "AUT SP:24.5 AC:OFF" is too long -> drop the "AC:" label
    snprintf(line2, sizeof(line2), "%s SP:%s %s", m, sp, s);
  }

  lcd.setCursor(0, 0);
  lcd.printf("%-16s", line1);
  lcd.setCursor(0, 1);
  lcd.printf("%-16.16s", line2);
}

// Publish the telemetry JSON
void publishTelemetry() {
  if (!mqtt.connected()) return;

  char tStr[12], hStr[12];
  if (isnan(temperature)) strcpy(tStr, "null");
  else snprintf(tStr, sizeof(tStr), "%.1f", temperature);
  if (isnan(humidity)) strcpy(hStr, "null");
  else snprintf(hStr, sizeof(hStr), "%.1f", humidity);

  char json[160];
  snprintf(json, sizeof(json),
           "{\"temperature\":%s,\"humidity\":%s,\"ac\":\"%s\","
           "\"mode\":\"%s\",\"setpoint\":%.1f}",
           tStr, hStr, acOn ? "ON" : "OFF", modeName(mode), setpoint);

  bool ok = mqtt.publish(TOPIC_TELEMETRY, json);
  Serial.printf("[MQTT]  PUB %s %s %s\n", TOPIC_TELEMETRY, json,
                ok ? "" : "(FAILED)");
}

// ----------------------------------------------------------------------------
//  MQTT: incoming commands
// ----------------------------------------------------------------------------

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  Serial.printf("[MQTT]  RECV %s -> \"%s\"\n", topic, msg.c_str());

  String upper = msg;
  upper.toUpperCase();
  bool changed = false;

  if (strcmp(topic, TOPIC_CMD_MODE) == 0) {
    if (upper == "AUTO") {
      mode = MODE_AUTO;
      changed = true;
    } else if (upper == "MANUAL") {
      mode = MODE_MANUAL;
      manualPower = acOn; // keep the current state when entering MANUAL
      changed = true;
    } else {
      Serial.println("[CMD]   Invalid mode (expected AUTO or MANUAL)");
    }
    if (changed) Serial.printf("[CMD]   Mode = %s\n", modeName(mode));

  } else if (strcmp(topic, TOPIC_CMD_POWER) == 0) {
    if (upper == "ON" || upper == "OFF") {
      if (mode != MODE_MANUAL) {
        mode = MODE_MANUAL; // a power command forces MANUAL mode
        Serial.println("[CMD]   Power command received -> switching to MANUAL");
      }
      manualPower = (upper == "ON");
      changed = true;
      Serial.printf("[CMD]   Manual power = %s\n", manualPower ? "ON" : "OFF");
    } else {
      Serial.println("[CMD]   Invalid power (expected ON or OFF)");
    }

  } else if (strcmp(topic, TOPIC_CMD_SETPOINT) == 0) {
    char *end = nullptr;
    float value = strtof(msg.c_str(), &end);
    bool isNumber = (end != msg.c_str()) && (*end == '\0');
    if (isNumber && value >= SETPOINT_MIN && value <= SETPOINT_MAX) {
      setpoint = value;
      changed = true;
      Serial.printf("[CMD]   Setpoint = %.1f C\n", setpoint);
    } else {
      Serial.printf("[CMD]   Setpoint rejected (must be %.0f..%.0f)\n",
                    SETPOINT_MIN, SETPOINT_MAX);
    }
  }

  if (changed) {
    updateControl();
    updateLcd();
    publishTelemetry(); // immediate feedback to the dashboard
  }
}

// ----------------------------------------------------------------------------
//  Connectivity (non-blocking)
// ----------------------------------------------------------------------------

void handleWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.printf("[WiFi]  Connected, IP = %s\n",
                    WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("[WiFi]  Connection lost");
  }

  unsigned long now = millis();
  if (now - lastWifiRetryMs >= WIFI_RETRY_INTERVAL) {
    lastWifiRetryMs = now;
    Serial.println("[WiFi]  Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  }
}

void handleMqtt() {
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  digitalWrite(MQTT_LED_PIN, LOW);
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (lastMqttRetryMs != 0 && now - lastMqttRetryMs < MQTT_RETRY_INTERVAL) return;
  lastMqttRetryMs = now;

  Serial.printf("[MQTT]  Connecting to %s:%u as %s ...\n", MQTT_HOST, MQTT_PORT,
                clientId);

  // Last Will: broker publishes "offline" (retained, QoS 1) if we disappear
  if (mqtt.connect(clientId, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("[MQTT]  Connected");
    digitalWrite(MQTT_LED_PIN, HIGH);

    mqtt.publish(TOPIC_STATUS, "online", true);
    Serial.printf("[MQTT]  PUB %s online (retained)\n", TOPIC_STATUS);

    mqtt.subscribe(TOPIC_CMD_MODE, 1);
    mqtt.subscribe(TOPIC_CMD_POWER, 1);
    mqtt.subscribe(TOPIC_CMD_SETPOINT, 1);
    Serial.printf("[MQTT]  SUB %s/cmd/{mode,power,setpoint}\n", TOPIC_BASE);

    publishTelemetry();
  } else {
    Serial.printf("[MQTT]  Failed, rc=%d, retry in %lus\n", mqtt.state(),
                  MQTT_RETRY_INTERVAL / 1000);
  }
}

// ----------------------------------------------------------------------------
//  Sensor
// ----------------------------------------------------------------------------

void handleSensor() {
  unsigned long now = millis();
  if (now - lastSensorMs < SENSOR_INTERVAL) return;
  lastSensorMs = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT]   Read error (NaN), reading skipped");
    return;
  }

  temperature = t;
  humidity = h;
  Serial.printf("[DHT]   T = %.1f C, H = %.1f %%\n", temperature, humidity);

  updateControl();
  updateLcd();
  publishTelemetry();
}

// ----------------------------------------------------------------------------
//  Arduino entry points
// ----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Smart Air Conditioner - Djebbar Lotfi ===");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(MQTT_LED_PIN, OUTPUT);
  digitalWrite(MQTT_LED_PIN, LOW);

  dht.begin();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart AC - IoT");
  lcd.setCursor(0, 1);
  lcd.print("Connecting...");

  // Unique MQTT client ID built from the chip MAC address
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "esp32-ac-%04X%08X",
           (uint16_t)(mac >> 32), (uint32_t)mac);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  lastWifiRetryMs = millis();
  Serial.printf("[WiFi]  Connecting to %s ...\n", WIFI_SSID);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
}

void loop() {
  handleWifi();
  handleMqtt();
  handleSensor();
}
