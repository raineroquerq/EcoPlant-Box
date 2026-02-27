#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <DHT.h>

// ================== WIFI ==================
#define WIFI_SSID "HUAWEI-5G-t4gQ"
#define WIFI_PASSWORD "bWV8s7gb"

// ================== FIREBASE ==================
#define API_KEY "AIzaSyBavEigwj3gkqYV-MNlKzrJfmD-QslPowM"
#define DATABASE_URL "https://irrigation-system-2a07f-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ================== OLED (SH1106) ==================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== PINS ==================
const int SOIL_PINS[6] = {32, 33, 34, 35, 36, 39};

// Relay ACTIVE LOW: LOW=ON, HIGH=OFF
#define RELAY_PIN 25

#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ================== CALIBRATION ==================
int dryRaw[6] = {2570, 2560, 2540, 2550, 2490, 2560};
int wetRaw[6] = {1050, 1240, 1240, 1050, 1060, 1050};

// ================== SETTINGS ==================
const int SAMPLES_PER_SENSOR = 10;
const unsigned long SENSOR_READ_MS = 2000;

const unsigned long HEARTBEAT_MS   = 5000;
const unsigned long UPLOAD_MS      = 5000;
const unsigned long RELAY_POLL_MS  = 1000;
const unsigned long SCHED_POLL_MS  = 5000;

// ===== AUTO WATERING (average soil moisture) =====
bool autoEnabled = true;          // set false if you only want app control
const int AUTO_ON_PCT  = 30;
const int AUTO_OFF_PCT = 42;      // stop watering at/above this (hysteresis)
const unsigned long AUTO_COOLDOWN_MS = 10UL * 60UL * 1000UL; // 10 minutes

unsigned long lastAutoRunMs = 0;

unsigned long pumpOnStartedMs = 0;
unsigned long totalPumpMs = 0;

unsigned long lastUsageUploadMs = 0;
const unsigned long USAGE_UPLOAD_INTERVAL = 5000;

// ================== FIREBASE OBJECTS ==================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// ================== STATE ==================
int soilPct[6] = {0, 0, 0, 0, 0, 0};
int moistureAvg = 0;

float tempC = NAN;
float humPct = NAN;

bool pumpIsOn = false;

// Schedule vars (from RTDB)
bool scheduleEnabled = false;
int scheduleHour = 0;
int scheduleMinute = 0;
int scheduleDurationMin = 0;

// Schedule runtime state
bool isScheduledRunning = false;
unsigned long scheduledStartMs = 0;
int lastExecutedMinute = -1;

// Timers
unsigned long lastSensorReadMs = 0;
unsigned long lastHeartbeatMs  = 0;
unsigned long lastUploadMs     = 0;
unsigned long lastRelayPollMs  = 0;
unsigned long lastSchedPollMs  = 0;

bool redraw = true;

// ================== HELPERS ==================
int readSoilAveraged(int pin) {
  long sum = 0;
  for (int i = 0; i < SAMPLES_PER_SENSOR; i++) {
    sum += analogRead(pin);
    delay(3);
  }
  return (int)(sum / SAMPLES_PER_SENSOR);
}

// Converts RAW to percent (0=dry, 100=wet)
int rawToPercent(int raw, int dry, int wet) {
  if (dry == wet) return 0;
  long pct = (long)(raw - dry) * 100L / (long)(wet - dry);
  pct = constrain(pct, 0, 100);
  return (int)pct;
}

void pumpOn() {
  if (pumpIsOn) return;
  digitalWrite(RELAY_PIN, LOW); // ACTIVE LOW
  pumpIsOn = true;
  pumpOnStartedMs = millis();
}

void pumpOff() {
  if (!pumpIsOn) return;
  digitalWrite(RELAY_PIN, HIGH);
  pumpIsOn = false;

  totalPumpMs += millis() - pumpOnStartedMs;
}

// ================== OLED UI ==================
void drawDashboard() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Header: Pump status
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("PUMP:");
  display.print(pumpIsOn ? "ON " : "OFF");

  // Divider
  display.drawLine(0, 16, 127, 16, SH110X_WHITE);

  // Soil percentages (two columns)
  const int y1 = 18, y2 = 28, y3 = 38;

  display.setCursor(0, y1);  display.print("S1 "); display.print(soilPct[0]); display.print("%");
  display.setCursor(0, y2);  display.print("S3 "); display.print(soilPct[2]); display.print("%");
  display.setCursor(0, y3);  display.print("S5 "); display.print(soilPct[4]); display.print("%");

  display.setCursor(64, y1); display.print("S2 "); display.print(soilPct[1]); display.print("%");
  display.setCursor(64, y2); display.print("S4 "); display.print(soilPct[3]); display.print("%");
  display.setCursor(64, y3); display.print("S6 "); display.print(soilPct[5]); display.print("%");

  // Bottom separator
  display.drawLine(0, 50, 127, 50, SH110X_WHITE);

  // Bottom text: Temp/Hum + Avg Moisture
  display.setCursor(0, 52);

  display.print("Temp:");
  if (isnan(tempC)) display.print("--");
  else { display.print(tempC, 0); display.print("C"); }

  display.print("  Hum:");
  if (isnan(humPct)) display.print("--");
  else { display.print(humPct, 0); display.print("%"); }

  display.display();
}

// ================== FIREBASE: READ SCHEDULE ==================
void fetchSchedule() {
  if (!signupOK || !Firebase.ready()) return;

  if (Firebase.RTDB.getJSON(&fbdo, "/devices/device1/schedules/main")) {
    FirebaseJson &json = fbdo.jsonObject();
    FirebaseJsonData r;

    json.get(r, "enabled");
    if (r.success) scheduleEnabled = r.boolValue;

    json.get(r, "hour");
    if (r.success) scheduleHour = r.intValue;

    json.get(r, "minute");
    if (r.success) scheduleMinute = r.intValue;

    json.get(r, "duration");
    if (r.success) scheduleDurationMin = r.intValue;

  } else {
    Serial.print("Schedule read failed: ");
    Serial.println(fbdo.errorReason());
  }
}

void handleAutoWatering() {
  if (!autoEnabled) return;

  // Don’t fight the schedule: if scheduled run is active, skip auto logic
  if (isScheduledRunning) return;

  // Cooldown after an auto run finishes (prevents rapid cycling)
  if (!pumpIsOn && (millis() - lastAutoRunMs) < AUTO_COOLDOWN_MS) return;

  // Start condition
  if (!pumpIsOn && moistureAvg < AUTO_ON_PCT) {
    Serial.println("AUTO: Pump ON (avg moisture low)");
    pumpOn();
    // optional: reflect to Firebase so app stays in sync
    Firebase.RTDB.setBool(&fbdo, "/devices/device1/relay", true);
  }

  // Stop condition
  if (pumpIsOn && moistureAvg >= AUTO_OFF_PCT) {
    Serial.println("AUTO: Pump OFF (avg moisture recovered)");
    pumpOff();
    lastAutoRunMs = millis();
    Firebase.RTDB.setBool(&fbdo, "/devices/device1/relay", false);
  }
}

// ================== FIREBASE: READ RELAY COMMAND ==================
void pollRelayCommand() {
  if (!signupOK || !Firebase.ready()) return;

  // If schedule is currently running, ignore manual relay command until it finishes
  if (isScheduledRunning) return;

  if (Firebase.RTDB.getBool(&fbdo, "/devices/device1/relay")) {
    bool relayState = fbdo.boolData();

    // Your relay is ACTIVE LOW: true = ON -> LOW
    if (relayState) pumpOn();
    else pumpOff();

  } else {
    Serial.print("Relay read failed: ");
    Serial.println(fbdo.errorReason());
  }
}

// ================== FIREBASE: UPLOAD SENSORS ==================
void uploadSensors() {
  if (!signupOK || !Firebase.ready()) return;

  // avg
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture", moistureAvg);

  // per-sensor (useful later for multi-pot UI)
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture1", soilPct[0]);
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture2", soilPct[1]);
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture3", soilPct[2]);
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture4", soilPct[3]);
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture5", soilPct[4]);
  Firebase.RTDB.setInt(&fbdo, "/devices/device1/sensors/moisture6", soilPct[5]);

  if (!isnan(tempC)) Firebase.RTDB.setFloat(&fbdo, "/devices/device1/sensors/temperature", tempC);
  if (!isnan(humPct)) Firebase.RTDB.setFloat(&fbdo, "/devices/device1/sensors/humidity", humPct);

  if (millis() - lastUsageUploadMs > USAGE_UPLOAD_INTERVAL) {

  lastUsageUploadMs = millis();

  unsigned long totalMinutes = totalPumpMs / 60000UL;

  if (Firebase.RTDB.setInt(&fbdo,
        "/devices/device1/usage/totalMinutes",
        (int)totalMinutes)) {

    Serial.print("Usage minutes: ");
    Serial.println(totalMinutes);

  } else {
    Serial.println("Usage upload failed");
    Serial.println(fbdo.errorReason());
  }
}
}

// ================== FIREBASE: HEARTBEAT ==================
void sendHeartbeat() {
  if (!signupOK || !Firebase.ready()) return;

  time_t now = time(nullptr);
  if (!Firebase.RTDB.setInt(&fbdo, "/devices/device1/lastSeen", (int)now)) {
    Serial.print("Heartbeat failed: ");
    Serial.println(fbdo.errorReason());
  }
}

// ================== SCHEDULE LOGIC ==================
void handleSchedule() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;

  // Start condition: first 10s of the scheduled minute, once per minute
  if (scheduleEnabled && !isScheduledRunning) {
    if (currentHour == scheduleHour &&
        currentMinute == scheduleMinute &&
        currentSecond < 10 &&
        currentMinute != lastExecutedMinute) {

      if (scheduleDurationMin > 0) {
        Serial.println("Scheduled irrigation started");
        pumpOn();

        isScheduledRunning = true;
        scheduledStartMs = millis();
        lastExecutedMinute = currentMinute;

        // Optional: reflect ON in Firebase so app stays in sync
        Firebase.RTDB.setBool(&fbdo, "/devices/device1/relay", true);
      }
    }
  }

  // Stop condition: duration minutes elapsed
  if (isScheduledRunning) {
    unsigned long runMs = (unsigned long)scheduleDurationMin * 60UL * 1000UL;
    if (millis() - scheduledStartMs >= runMs) {
      Serial.println("Scheduled irrigation stopped");
      pumpOff();
      isScheduledRunning = false;

      // Optional: reflect OFF in Firebase
      Firebase.RTDB.setBool(&fbdo, "/devices/device1/relay", false);

      // Optional: write history marker for calendar
      // Format YYYY-MM-DD
      char dateKey[11];
      snprintf(dateKey, sizeof(dateKey), "%04d-%02d-%02d",
               timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

      Firebase.RTDB.setString(&fbdo, String("/devices/device1/waterHistory/") + dateKey, "watered");
    }
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  pumpOff();

  // ADC
  analogReadResolution(12);
  for (int i = 0; i < 6; i++) {
    analogSetPinAttenuation(SOIL_PINS[i], ADC_11db);
  }

  // DHT
  dht.begin();

  // OLED
  Wire.begin(21, 22);
  if (!display.begin(0x3C, true)) {
    Serial.println("SH1106 init failed at 0x3C. Try 0x3D if needed.");
    while (true) {}
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Booting...");
  display.display();

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  // NTP (Philippines UTC+8)
  configTime(8 * 3600, 0, "pool.ntp.org");

  Serial.println("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(300);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synchronized!");

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase SignUp OK");
    signupOK = true;
  } else {
    Serial.printf("SignUp Failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Firebase + UI OK");
  display.display();
  delay(600);
}

// ================== LOOP ==================
void loop() {
  unsigned long ms = millis();

  // ---- Read sensors (local) ----
  if (ms - lastSensorReadMs >= SENSOR_READ_MS) {
    lastSensorReadMs = ms;

    long sum = 0;
    for (int i = 0; i < 6; i++) {
      int raw = readSoilAveraged(SOIL_PINS[i]);
      soilPct[i] = rawToPercent(raw, dryRaw[i], wetRaw[i]);
      sum += soilPct[i];
    }
    moistureAvg = (int)(sum / 6);

    tempC = dht.readTemperature();
    humPct = dht.readHumidity();

    redraw = true;
  }

  handleAutoWatering();

  // ---- Firebase tasks ----
  if (signupOK && Firebase.ready()) {
    if (ms - lastHeartbeatMs >= HEARTBEAT_MS) {
      lastHeartbeatMs = ms;
      sendHeartbeat();
    }

    if (ms - lastSchedPollMs >= SCHED_POLL_MS) {
      lastSchedPollMs = ms;
      fetchSchedule();
    }

    // Schedule start/stop runs locally using NTP time
    handleSchedule();

    if (ms - lastRelayPollMs >= RELAY_POLL_MS) {
      lastRelayPollMs = ms;
      pollRelayCommand();
    }

    if (ms - lastUploadMs >= UPLOAD_MS) {
      lastUploadMs = ms;
      uploadSensors();
    }
  }

  // ---- OLED ----
  if (redraw) {
    redraw = false;
    drawDashboard();
  }
}
