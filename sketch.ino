/*
 * ============================================================================
 *  IoT-Based Smart Greenhouse & Automated Climate Monitoring System
 *  Course   : Internet of Things (CS-481) - Problem-Based Learning (CLO-3)
 *  Dept.    : Computer Systems Engineering, MUET Jamshoro
 *  Platform : Wokwi Simulator  |  Board: ESP32 DevKit V1
 * ----------------------------------------------------------------------------
 *  What this firmware does
 *   1. Reads temperature + humidity (DHT22) and soil moisture (potentiometer).
 *   2. Shows live data and system status on a 128x64 SSD1306 OLED (I2C).
 *   3. Edge logic (works even with NO internet):
 *        - Temp > 35 C  -> Fan relay ON  + buzzer alarm
 *        - Soil < 30 %  -> Water-pump relay ON
 *      Hysteresis is used so actuators do not chatter around a threshold.
 *   4. Connects to (virtual) Wi-Fi and publishes JSON telemetry over MQTT to
 *      broker.hivemq.com. The SAME JSON is also printed on the Serial Monitor.
 *   5. BONUS: subscribes to a command topic so the greenhouse can be
 *      controlled remotely (auto / fan_on / fan_off / pump_on / pump_off).
 *
 *  Pin map
 *   DHT22 SDA -> GPIO15      Potentiometer SIG -> GPIO34 (ADC1, input-only)
 *   OLED SDA  -> GPIO21      OLED SCL          -> GPIO22
 *   Fan relay IN -> GPIO26   Pump relay IN     -> GPIO27
 *   Buzzer(+) -> GPIO25
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ------------------------------ Pin definitions -----------------------------
#define DHT_PIN         15
#define DHT_TYPE        DHT22
#define SOIL_PIN        34      // potentiometer simulating soil-moisture probe
#define FAN_RELAY_PIN   26
#define PUMP_RELAY_PIN  27
#define BUZZER_PIN      25
#define I2C_SDA         21
#define I2C_SCL         22

// Wokwi relay module energises when IN = HIGH.
// If you use a real ACTIVE-LOW relay board, swap these two values.
#define RELAY_ON        HIGH
#define RELAY_OFF       LOW

// ------------------------------ OLED settings -------------------------------
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_ADDRESS    0x3C

// ------------------------------ Thresholds ----------------------------------
const float TEMP_HIGH_C   = 35.0;   // fan ON + alarm above this
const float TEMP_HYST_C   = 2.0;    // fan OFF again below (35 - 2) = 33 C
const int   SOIL_DRY_PCT  = 30;     // pump ON below this
const int   SOIL_WET_PCT  = 40;     // pump OFF again at/above this

// ------------------------------ Timing (ms) ---------------------------------
const unsigned long SENSOR_INTERVAL_MS  = 2000;   // DHT22 needs >= 2 s
const unsigned long PUBLISH_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_MS       = 5000;
const unsigned long MQTT_RETRY_MS       = 5000;
const unsigned long BEEP_INTERVAL_MS    = 400;

// ------------------------------ Network / MQTT ------------------------------
const char* WIFI_SSID    = "Wokwi-GUEST";   // Wokwi's built-in virtual access point
const char* WIFI_PASS    = "";
const int   WIFI_CHANNEL = 6;               // speeds up the connection in Wokwi

const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;
const char*    DEVICE_ID   = "greenhouse-01";

// Public broker = shared by everyone, so keep a unique prefix.
// Change "23cs_yourroll" to something unique to you.
const char* TOPIC_TELEMETRY = "muet/cs481/23cs_yourroll/greenhouse/telemetry";
const char* TOPIC_STATUS    = "muet/cs481/23cs_yourroll/greenhouse/status";
const char* TOPIC_CMD       = "muet/cs481/23cs_yourroll/greenhouse/cmd";

// ------------------------------ Objects -------------------------------------
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ------------------------------ System state --------------------------------
enum Mode { MODE_AUTO, MODE_MANUAL };

float temperature = 0.0;
float humidity    = 0.0;
int   soilPercent = 0;
bool  sensorOk    = false;

bool fanOn    = false;
bool pumpOn   = false;
bool alarmOn  = false;
Mode mode     = MODE_AUTO;

bool          buzzerState   = false;
unsigned long lastBeepMs    = 0;
unsigned long lastSensorMs  = 0;
unsigned long lastPublishMs = 0;
unsigned long lastWifiTryMs = 0;
unsigned long lastMqttTryMs = 0;

// =============================================================================
//  Actuator helpers
// =============================================================================
void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

void applyOutputs() {
  setRelay(FAN_RELAY_PIN, fanOn);
  setRelay(PUMP_RELAY_PIN, pumpOn);
}

// Non-blocking buzzer: beeps on/off while the alarm is active.
void handleBuzzer(unsigned long now) {
  if (!alarmOn) {
    if (buzzerState) {
      noTone(BUZZER_PIN);
      buzzerState = false;
    }
    return;
  }
  if (now - lastBeepMs >= BEEP_INTERVAL_MS) {
    lastBeepMs = now;
    buzzerState = !buzzerState;
    if (buzzerState) tone(BUZZER_PIN, 2000);
    else             noTone(BUZZER_PIN);
  }
}

// =============================================================================
//  Sensors
// =============================================================================
int readSoilPercent() {
  // Average 10 samples to reduce ADC noise. 0 = bone dry, 100 = fully wet.
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(SOIL_PIN);
    delayMicroseconds(200);
  }
  int raw = sum / 10;                                   // 0..4095 (12-bit)
  return constrain(map(raw, 0, 4095, 0, 100), 0, 100);
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {          // fail-safe: keep last good values
    sensorOk = false;
    Serial.println("[WARN] DHT22 read failed");
  } else {
    sensorOk    = true;
    temperature = t;
    humidity    = h;
  }
  soilPercent = readSoilPercent();
}

// =============================================================================
//  Edge control logic (runs locally on the ESP32 - no cloud needed)
// =============================================================================
void runControlLogic() {
  // --- Temperature alarm with hysteresis (always active = safety feature) ---
  if (sensorOk) {
    if (temperature > TEMP_HIGH_C)                      alarmOn = true;
    else if (temperature < TEMP_HIGH_C - TEMP_HYST_C)   alarmOn = false;
  }

  if (mode == MODE_AUTO) {
    // Fan follows the high-temperature condition
    fanOn = alarmOn;

    // Pump: ON when soil is dry, OFF once it is wet enough again
    if (soilPercent < SOIL_DRY_PCT)        pumpOn = true;
    else if (soilPercent >= SOIL_WET_PCT)  pumpOn = false;
  }
  // In MODE_MANUAL the fan/pump states are set by MQTT commands instead.

  applyOutputs();
}

// =============================================================================
//  OLED display
// =============================================================================
void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Row 0: connectivity + mode
  display.setCursor(0, 0);
  display.print("WiFi:");
  display.print(WiFi.status() == WL_CONNECTED ? "OK" : "--");
  display.print(" MQTT:");
  display.print(mqtt.connected() ? "OK" : "--");
  display.print(mode == MODE_AUTO ? " AUTO" : " MAN");

  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

  // Rows 1-3: readings
  display.setCursor(0, 13);
  display.print("Temp : ");
  if (sensorOk) { display.print(temperature, 1); display.print(" C"); }
  else          { display.print("ERR"); }

  display.setCursor(0, 23);
  display.print("Humid: ");
  if (sensorOk) { display.print(humidity, 1); display.print(" %"); }
  else          { display.print("ERR"); }

  display.setCursor(0, 33);
  display.print("Soil : ");
  display.print(soilPercent);
  display.print(" %");

  // Row 4: actuators
  display.setCursor(0, 43);
  display.print("Fan:");
  display.print(fanOn ? "ON " : "OFF");
  display.print(" Pump:");
  display.print(pumpOn ? "ON" : "OFF");

  // Row 5: system status (inverted bar when critical)
  display.setCursor(0, 55);
  if (alarmOn) {
    display.fillRect(0, 53, SCREEN_WIDTH, 11, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, 55);
    display.print("!! HIGH TEMP ALERT !!");
    display.setTextColor(SSD1306_WHITE);
  } else if (soilPercent < SOIL_DRY_PCT) {
    display.print("Status: SOIL DRY");
  } else {
    display.print("Status: NORMAL");
  }

  display.display();
}

// =============================================================================
//  IoT: Wi-Fi + MQTT
// =============================================================================
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected!");
    Serial.print("[WiFi] IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" failed (will retry in background)");
  }
}

// Handles remote commands received on TOPIC_CMD
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[24];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  for (char* p = msg; *p; ++p) *p = tolower(*p);

  Serial.print("[MQTT] Command received: ");
  Serial.println(msg);

  if      (!strcmp(msg, "auto"))     { mode = MODE_AUTO; }
  else if (!strcmp(msg, "fan_on"))   { mode = MODE_MANUAL; fanOn  = true;  }
  else if (!strcmp(msg, "fan_off"))  { mode = MODE_MANUAL; fanOn  = false; }
  else if (!strcmp(msg, "pump_on"))  { mode = MODE_MANUAL; pumpOn = true;  }
  else if (!strcmp(msg, "pump_off")) { mode = MODE_MANUAL; pumpOn = false; }
  else { Serial.println("[MQTT] Unknown command ignored"); return; }

  runControlLogic();
  updateDisplay();
}

void connectMqtt() {
  // Random client-id so two students on the public broker do not clash
  char clientId[40];
  snprintf(clientId, sizeof(clientId), "%s-%04X", DEVICE_ID, (unsigned)random(0xFFFF));

  Serial.print("[MQTT] Connecting to ");
  Serial.print(MQTT_BROKER);
  Serial.print(" ... ");

  // Last-Will message: broker publishes "offline" if we drop unexpectedly
  if (mqtt.connect(clientId, NULL, NULL, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD);
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqtt.state());
  }
}

// Called regularly from loop(); never blocks for long.
void maintainConnections(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTryMs >= WIFI_RETRY_MS) {
      lastWifiTryMs = now;
      Serial.println("[WiFi] Lost connection - retrying");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
    }
    return;
  }
  if (!mqtt.connected() && now - lastMqttTryMs >= MQTT_RETRY_MS) {
    lastMqttTryMs = now;
    connectMqtt();
  }
}

// Builds ONE structured JSON payload; used for MQTT and Serial Monitor.
void publishTelemetry() {
  char json[320];
  snprintf(json, sizeof(json),
    "{\"device\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,"
    "\"soil_moisture\":%d,\"fan\":\"%s\",\"pump\":\"%s\","
    "\"alarm\":%s,\"mode\":\"%s\",\"wifi_rssi\":%d,\"uptime_s\":%lu}",
    DEVICE_ID, temperature, humidity, soilPercent,
    fanOn ? "ON" : "OFF", pumpOn ? "ON" : "OFF",
    alarmOn ? "true" : "false",
    mode == MODE_AUTO ? "AUTO" : "MANUAL",
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    millis() / 1000UL);

  Serial.print("[JSON] ");
  Serial.println(json);                       // Option B: Serial JSON payload

  if (mqtt.connected()) {                     // Option A: MQTT publish
    if (mqtt.publish(TOPIC_TELEMETRY, json)) Serial.println("[MQTT] Published OK");
    else                                     Serial.println("[MQTT] Publish failed");
  }
}

// =============================================================================
//  Arduino entry points
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Smart Greenhouse - ESP32 booting ===");

  pinMode(FAN_RELAY_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  fanOn = false; pumpOn = false;
  applyOutputs();                              // start with everything OFF

  analogReadResolution(12);
  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[ERROR] SSD1306 not found - check I2C wiring");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 14);
    display.println("SMART GREENHOUSE");
    display.setCursor(10, 30);
    display.println("IoT PBL - CS-481");
    display.setCursor(10, 46);
    display.println("Connecting...");
    display.display();
  }

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);
  if (WiFi.status() == WL_CONNECTED) connectMqtt();

  delay(1500);                                 // let DHT22 settle
  readSensors();
  runControlLogic();
  updateDisplay();
  publishTelemetry();
  lastSensorMs  = millis();
  lastPublishMs = millis();
}

void loop() {
  unsigned long now = millis();

  maintainConnections(now);
  if (mqtt.connected()) mqtt.loop();

  // --- periodic sensing + control + display ---
  if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = now;

    bool prevFan = fanOn, prevPump = pumpOn;
    readSensors();
    runControlLogic();
    updateDisplay();

    if (fanOn != prevFan || pumpOn != prevPump) {
      Serial.println(fanOn != prevFan ? (fanOn ? "[EVENT] Fan ON" : "[EVENT] Fan OFF")
                                      : (pumpOn ? "[EVENT] Pump ON" : "[EVENT] Pump OFF"));
      publishTelemetry();                      // report state changes immediately
      lastPublishMs = now;
    }
  }

  // --- periodic telemetry ---
  if (now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishTelemetry();
  }

  handleBuzzer(now);                           // non-blocking alarm beeps
}
