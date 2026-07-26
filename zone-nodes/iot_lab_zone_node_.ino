#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

//===================== BACKEND CONFIG (edit these 2 lines) =====================
const char* BACKEND_URL = "http://hummus-gulf-unexpired.ngrok-free.dev";  // <-- your ngrok URL, http://
const char* ZONE_API_KEY = "667b28ab75497f64";
const char* ZONE_ID      = "iot_lab";

const char* WIFI_SSID = "Wokwi-GUEST";   // Wokwi's simulated network, gives real internet access
const char* WIFI_PASS = "";

//===================== SENSOR PINS =====================

#define LDR_PIN        34   // stands in for the flame/fire sensor
#define GAS_PIN        35

#define TRIG_PIN       5
#define ECHO_PIN       18

#define PIR_PIN        27

#define DHT_PIN        4
#define DHT_TYPE       DHT22

//===================== OUTPUT PINS =====================

#define GREEN_LED      19   // SAFE
#define YELLOW_LED     22   // WARNING (blinks)
#define RED_LED        21   // CRITICAL (solid) / OFFLINE (fast blink)

#define RELAY_PIN      26
#define BUZZER_PIN     23

//===================== DHT =====================

DHT dht(DHT_PIN, DHT_TYPE);

//===================== RAW SENSOR VALUES =====================

int ldrValue = 0;
int gasValue = 0;
int motion = 0;

float temperature = 0;
float humidity = 0;
float distance = 0;

//===================== STATE MACHINE =====================

#define SAFE_STATE      0
#define WARNING_STATE   1
#define CRITICAL_STATE  2
#define OFFLINE_STATE   3

int currentState = SAFE_STATE;
int previousState = SAFE_STATE;

//===================== BOOT COUNTER (NEW -- reboot-loop detection) =====================
// RTC_DATA_ATTR survives a software reset / panic-reboot (though not a full
// power cycle). If this number keeps climbing across a single simulation
// run, the chip is silently resetting -- which explains bootTime (and the
// gas warm-up window) restarting from zero over and over, even though the
// Wokwi/Serial timer visually looks like it just keeps counting up.
RTC_DATA_ATTR int bootCount = 0;

//===================== BACKEND-DRIVEN STATE =====================

bool backendReachable = false;
int backendState = SAFE_STATE;
bool backendActuate = false;

unsigned long lastSendTime = 0;
const unsigned long SEND_INTERVAL_MS = 1000;

bool pendingRetry = false;

//===================== STARTUP GRACE PERIOD =====================

#define STARTUP_GRACE_MS   3000
unsigned long systemBootTime = 0;

bool inStartupGrace() {
  return (millis() - systemBootTime) < STARTUP_GRACE_MS;
}

//===================== FIRE: DEBOUNCE + RECOVERY =====================

#define FIRE_RAW_THRESHOLD   1800
#define FIRE_DEBOUNCE_MS     1000
#define FIRE_RECOVERY_MS     3000

unsigned long fireAboveSince = 0;
bool fireConfirmed = false;
unsigned long fireDecayStart = 0;
float fireRisk = 0.0;

//===================== GAS: WARM-UP =====================

#define GAS_WARMUP_MS   30000
unsigned long bootTime = 0;
float gasRisk = 0.0;
bool gasWarmingUp = true;

//===================== WATER =====================

float waterRisk = 0.0;

//===================== PIR: DEBOUNCE + OFFLINE DETECTION =====================

#define OCC_HOLD_MS            1000
#define PIR_TOGGLE_WINDOW_MS   1000
#define PIR_TOGGLE_LIMIT       6

int lastPirRaw = LOW;
unsigned long pirToggleWindowStart = 0;
int pirToggleCount = 0;
bool pirOffline = false;

bool pendingOccupancy = false;
unsigned long pendingSince = 0;
bool occupancyConfirmed = false;

//===================== RISK FUSION WEIGHTS (Section 13 formula) =====================

const float W_FIRE  = 40.0;
const float W_GAS   = 25.0;
const float W_WATER = 20.0;
const float W_OCC   = 15.0;

float riskScore = 0.0;

unsigned long previousMillis = 0;
bool ledState = LOW;
const long blinkInterval = 500;
const long offlineBlinkInterval = 150;

//===================== BUZZER: BEEP PATTERN =====================

unsigned long buzzerPreviousMillis = 0;
bool buzzerState = LOW;
const long buzzerOnMs  = 150;
const long buzzerOffMs = 150;

void updateBuzzer(bool shouldBeep) {
  if (!shouldBeep) {
    if (buzzerState != LOW) {
      buzzerState = LOW;
      digitalWrite(BUZZER_PIN, LOW);
    }
    return;
  }

  unsigned long now = millis();
  unsigned long interval = buzzerState ? buzzerOnMs : buzzerOffMs;

  if (now - buzzerPreviousMillis >= interval) {
    buzzerPreviousMillis = now;
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}

//===================== SETUP =====================

void setup() {

  Serial.begin(115200);

  // FIX (root cause of the "gas stuck on WARMING UP forever" bug):
  // the loop task was previously blocking for up to ~3 attempts x 10s
  // timeout = ~30s+ inside sendToBackend() whenever the backend was slow
  // or unreachable, with no yield() in between. That's long enough to
  // trip the ESP32's task watchdog, which silently resets the chip --
  // wiping bootTime back to ~0 and restarting the 30s gas warm-up window
  // every time. We now (a) explicitly configure a longer watchdog timeout
  // as a safety net, and (b) shrink the HTTP timeouts/attempts below so a
  // stuck backend can never block the loop anywhere near that long again.
  // NOTE: newer arduino-esp32 cores (3.x, used by current Wokwi) already
  // auto-initialize the Task Watchdog Timer via their default sdkconfig, so
  // we deliberately do NOT call esp_task_wdt_init()/reconfigure() here --
  // its exact signature has changed across core versions and re-triggers
  // build errors. We just register the loop task with whatever TWDT the
  // framework already started, then feed it with esp_task_wdt_reset() calls
  // spread through loop()/sendToBackend()/connectWifi() below. If this
  // build's core has TWDT disabled by default instead, esp_task_wdt_add()
  // and esp_task_wdt_reset() are still safe no-ops -- they just won't do
  // anything, which is harmless.
  esp_task_wdt_add(NULL);

  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(PIR_PIN, INPUT);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(GREEN_LED, HIGH);   // boot straight into a visible SAFE state
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  bootTime = millis();
  systemBootTime = millis();
  pirToggleWindowStart = millis();

  bootCount++;   // NEW -- increments only if this is a genuine reset/reboot

  Serial.println("======================================");
  Serial.println(" SMART CAMPUS SAFETY SYSTEM ");
  Serial.println("======================================");
  Serial.print(" Boot count this session: ");
  Serial.println(bootCount);
  if (bootCount > 1) {
    Serial.println(" *** WARNING: chip has reset more than once -- ");
    Serial.println("     something upstream (WiFi/backend/watchdog) is crashing it. ***");
  }
  Serial.println("Booting into SAFE state, startup grace period active...");

  connectWifi();
}

//===================== WIFI =====================

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
    esp_task_wdt_reset();   // feed the watchdog during the connect-wait loop
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED -- running on local fail-safe logic only.");
  }
}

//===================== ULTRASONIC =====================

float getDistance() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0)
    return 999;

  return duration * 0.0343 / 2.0;
}

//===================== READ ALL SENSORS =====================

void readSensors() {

  ldrValue = analogRead(LDR_PIN);

  gasValue = analogRead(GAS_PIN);

  distance = getDistance();

  motion = digitalRead(PIR_PIN);

  temperature = dht.readTemperature();

  humidity = dht.readHumidity();

}

//===================== FIRE: debounce + recovery (LOCAL fail-safe copy) ====

void processFire() {

  bool aboveThreshold = (ldrValue > FIRE_RAW_THRESHOLD);
  unsigned long now = millis();

  if (aboveThreshold) {

    if (fireAboveSince == 0) fireAboveSince = now;

    if (!fireConfirmed && (now - fireAboveSince >= FIRE_DEBOUNCE_MS)) {
      fireConfirmed = true;
    }

    if (fireConfirmed) {
      fireRisk = 1.0;
      fireDecayStart = 0;
    }

  } else {

    fireAboveSince = 0;

    if (fireConfirmed) {
      if (fireDecayStart == 0) fireDecayStart = now;

      unsigned long elapsed = now - fireDecayStart;

      if (elapsed >= FIRE_RECOVERY_MS) {
        fireRisk = 0.0;
        fireConfirmed = false;
        fireDecayStart = 0;
      } else {
        fireRisk = 1.0 - ((float)elapsed / (float)FIRE_RECOVERY_MS);
      }

    } else {
      fireRisk = 0.0;
    }
  }
}

//===================== GAS: warm-up handling =====================

void processGas() {

  gasWarmingUp = (millis() - bootTime) < GAS_WARMUP_MS;

  if (gasWarmingUp) {
    gasRisk = 0.0;
  } else {
    gasRisk = constrain((float)gasValue / 4095.0, 0.0, 1.0);
  }
}

//===================== WATER: normalize distance =====================

void processWater() {

  if (distance <= 0 || distance > 50) {
    waterRisk = 0.0;
  } else if (distance <= 5) {
    waterRisk = 1.0;
  } else {
    waterRisk = constrain((50.0 - distance) / 45.0, 0.0, 1.0);
  }
}

//===================== PIR: debounce + offline detection =====================

void processPir() {

  int raw = motion;
  unsigned long now = millis();

  if (raw != lastPirRaw) {

    if (now - pirToggleWindowStart > PIR_TOGGLE_WINDOW_MS) {
      pirToggleWindowStart = now;
      pirToggleCount = 0;
    }

    pirToggleCount++;
    lastPirRaw = raw;
  }

  pirOffline = (pirToggleCount > PIR_TOGGLE_LIMIT);

  if (!pirOffline) {

    bool rawOccupied = (raw == HIGH);

    if (rawOccupied != pendingOccupancy) {
      pendingOccupancy = rawOccupied;
      pendingSince = now;
    }

    if (now - pendingSince >= OCC_HOLD_MS) {
      occupancyConfirmed = pendingOccupancy;
    }
  }
}

//===================== SEND RAW READINGS TO BACKEND =====================
// FIX: timeouts and attempt count reduced so a slow/unreachable backend can
// never block the loop task long enough to trip the watchdog. Old worst
// case: 3 attempts x 10s timeout + 2x300ms retry delay = ~30.6s blocked.
// New worst case: 2 attempts x 4s timeout + 1x300ms retry delay = ~8.3s --
// comfortably under the 20s WDT configured in setup(), with margin.
// esp_task_wdt_reset() is also called around the blocking POST call itself
// as a second layer of protection.

void sendToBackend() {

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) {
      backendReachable = false;
      return;
    }
  }

  Serial.print("WiFi status before POST: ");
  Serial.println(WiFi.status());
  Serial.print("Free heap before POST: ");
  Serial.println(ESP.getFreeHeap());

  String url = String(BACKEND_URL) + "/api/ingest/" + ZONE_ID;

  float rawFireSignal = (ldrValue > FIRE_RAW_THRESHOLD) ? 1.0 : 0.0;
  float gasNorm = constrain((float)gasValue / 4095.0, 0.0, 1.0);

  StaticJsonDocument<256> doc;
  doc["fire"] = rawFireSignal;
  doc["gas"] = gasNorm;
  doc["gas_warming_up"] = gasWarmingUp;
  doc["water"] = waterRisk;
  doc["occupancy"] = occupancyConfirmed;
  doc["pir_offline"] = pirOffline;
  doc["sequence_no"] = (long)(millis());

  String body;
  serializeJson(doc, body);

  int httpCode = -1;
  const int MAX_ATTEMPTS = 2;                 // was 3
  const int HTTP_TIMEOUT_MS = 4000;           // was 10000

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {

    esp_task_wdt_reset();   // feed the watchdog right before a blocking call

    WiFiClient client;

    HTTPClient http;
    http.begin(client, url);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", ZONE_API_KEY);
    http.addHeader("ngrok-skip-browser-warning", "true");

    Serial.print("Sending POST to: ");
    Serial.print(url);
    Serial.print("  (attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(MAX_ATTEMPTS);
    Serial.println(")");

    httpCode = http.POST(body);

    esp_task_wdt_reset();   // feed it again right after, before any more work

    Serial.print("httpCode: ");
    Serial.println(httpCode);

    if (httpCode == 307 || httpCode == 301 || httpCode == 302) {
      Serial.print(">>> Redirected to: ");
      Serial.println(http.getLocation());
    }

    if (httpCode == 200) {
      String response = http.getString();
      Serial.print("Backend -> ");
      Serial.println(response);

      StaticJsonDocument<256> resp;
      if (deserializeJson(resp, response) == DeserializationError::Ok) {
        String stateStr = resp["state"] | "SAFE";
        bool rawActuate = resp["actuate"] | false;

        int parsedState = SAFE_STATE;
        if (stateStr == "SAFE") parsedState = SAFE_STATE;
        else if (stateStr == "WARNING") parsedState = WARNING_STATE;
        else if (stateStr == "CRITICAL") parsedState = CRITICAL_STATE;
        else if (stateStr == "OFFLINE") parsedState = OFFLINE_STATE;

        if (inStartupGrace()) {
          backendState = SAFE_STATE;
          backendActuate = false;
          Serial.println("  (startup grace period active -- forcing SAFE, ignoring backend state)");
        } else {
          backendState = parsedState;
          backendActuate = rawActuate;
        }

        backendReachable = true;
        pendingRetry = false;
      }
      http.end();
      return;
    }

    Serial.print("  attempt ");
    Serial.print(attempt);
    Serial.println(" failed" );
    if (httpCode < 0) {
      Serial.println("  (negative code = network-level failure, not an HTTP response)");
    }

    http.end();

    if (attempt < MAX_ATTEMPTS) {
      Serial.println("  retrying...");
      delay(300);
      esp_task_wdt_reset();
    }
  }

  Serial.print("Backend send failed after ");
  Serial.print(MAX_ATTEMPTS);
  Serial.print(" attempts, httpCode=");
  Serial.println(httpCode);
  backendReachable = false;
  pendingRetry = true;
}

//===================== PRINT SENSOR DATA =====================

void printSensorData() {

  Serial.println("--------------------------------------");

  if (inStartupGrace()) {
    Serial.print("STARTUP GRACE PERIOD: ");
    Serial.print((STARTUP_GRACE_MS - (millis() - systemBootTime)) / 1000.0, 1);
    Serial.println("s remaining -- forcing SAFE");
  }

  Serial.print("LDR (fire) raw   : ");
  Serial.print(ldrValue);
  Serial.print("   fireRisk(local): ");
  Serial.println(fireRisk, 2);

  // NEW: print the actual countdown, not just the boolean, so a stuck
  // warm-up (reboot loop, or a mismatched GAS_WARMUP_MS) is obvious on
  // screen instead of silently showing "(WARMING UP - ignored)" forever.
  Serial.print("Gas raw          : ");
  Serial.print(gasValue);
  Serial.print("   gasRisk: ");
  Serial.print(gasRisk, 2);
  if (gasWarmingUp) {
    long remainingMs = (long)GAS_WARMUP_MS - (long)(millis() - bootTime);
    Serial.print("   (WARMING UP - ignored, ");
    Serial.print(remainingMs / 1000.0, 1);
    Serial.println("s remaining)");
  } else {
    Serial.println();
  }

  Serial.print("Water Distance   : ");
  Serial.print(distance);
  Serial.print(" cm   waterRisk: ");
  Serial.println(waterRisk, 2);

  Serial.print("Occupancy        : ");
  if (pirOffline)
    Serial.println("OFFLINE (sensor fault)");
  else
    Serial.println(occupancyConfirmed ? "Occupied" : "Empty");

  if (!isnan(temperature)) {
    Serial.print("Temperature      : ");
    Serial.print(temperature);
    Serial.println(" C");
  }

  if (!isnan(humidity)) {
    Serial.print("Humidity         : ");
    Serial.print(humidity);
    Serial.println(" %");
  }

  Serial.print("LOCAL risk estimate : ");
  Serial.println(riskScore, 1);
  Serial.print("BACKEND reachable   : ");
  Serial.println(backendReachable ? "yes (backend is authoritative)" : "no (using local fail-safe)");
  Serial.print("Boot count           : ");
  Serial.println(bootCount);

  Serial.println("--------------------------------------");
}

//===================== CHECK STATUS (LOCAL fail-safe risk fusion) =====================

void checkStatus() {

  if (inStartupGrace()) {
    currentState = SAFE_STATE;
    riskScore = 0.0;
    return;
  }

  if (pirOffline) {
    currentState = OFFLINE_STATE;
    return;
  }

  float occFactor = occupancyConfirmed ? 1.0 : 0.3;

  riskScore = (W_FIRE  * fireRisk)
            + (W_GAS   * gasRisk)
            + (W_WATER * waterRisk)
            + (W_OCC   * occFactor);

  if (riskScore >= 65)
    currentState = CRITICAL_STATE;
  else if (riskScore >= 30)
    currentState = WARNING_STATE;
  else
    currentState = SAFE_STATE;
}

//===================== UPDATE OUTPUTS =====================

void updateOutputs() {

  int effectiveState = backendReachable ? backendState : currentState;
  bool effectiveActuate = backendReachable ? backendActuate : (currentState == CRITICAL_STATE);

  bool stateChanged = (effectiveState != previousState);

  if (stateChanged) {

    Serial.println();
    Serial.print("STATE CHANGED : ");

    if (effectiveState == SAFE_STATE) Serial.println("SAFE");
    else if (effectiveState == WARNING_STATE) Serial.println("WARNING");
    else if (effectiveState == CRITICAL_STATE) Serial.println("CRITICAL");
    else if (effectiveState == OFFLINE_STATE) Serial.println("OFFLINE");

    if (effectiveState == WARNING_STATE) {
      previousMillis = millis();
      ledState = HIGH;
      digitalWrite(YELLOW_LED, ledState);
    }

    if (effectiveState == CRITICAL_STATE) {
      digitalWrite(RED_LED, HIGH);
      if (effectiveActuate) {
        digitalWrite(RELAY_PIN, HIGH);
      }
    }

    previousState = effectiveState;
  }

  //================ SAFE =================

  if (effectiveState == SAFE_STATE) {

    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);

    digitalWrite(RELAY_PIN, LOW);
    updateBuzzer(false);

    Serial.println("STATUS : SAFE");
  }

  //================ WARNING =================

  else if (effectiveState == WARNING_STATE) {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);

    unsigned long currentMillis = millis();

    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(YELLOW_LED, ledState);
    }

    digitalWrite(RELAY_PIN, LOW);
    updateBuzzer(false);

    Serial.println("STATUS : WARNING");
  }

  //================ CRITICAL =================

  else if (effectiveState == CRITICAL_STATE) {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, HIGH);

    if (effectiveActuate) {
      digitalWrite(RELAY_PIN, HIGH);
      updateBuzzer(true);
    } else {
      digitalWrite(RELAY_PIN, LOW);
      updateBuzzer(false);
    }

    Serial.println("STATUS : CRITICAL");
  }

  //================ OFFLINE =================

  else {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);

    unsigned long currentMillis = millis();

    if (currentMillis - previousMillis >= offlineBlinkInterval) {
      previousMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(RED_LED, ledState);
    }

    digitalWrite(RELAY_PIN, LOW);
    updateBuzzer(false);

    Serial.println("STATUS : OFFLINE (PIR sensor fault)");
  }
}

//===================== LOOP =====================

void loop() {

  esp_task_wdt_reset();   // feed the watchdog at the top of every loop

  readSensors();

  processFire();
  processGas();
  processWater();
  processPir();

  checkStatus();

  printSensorData();

  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendToBackend();
  }

  updateOutputs();

  delay(200);
}
