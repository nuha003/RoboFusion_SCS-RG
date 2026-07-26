#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

//===================== BACKEND CONFIG (edit these 2 lines) =====================
// IMPORTANT: BACKEND_URL must now be a plain http:// ngrok URL
// (started with `ngrok http 8000 --scheme=http`), NOT https://.
// Wokwi's simulated network fails the TLS handshake against ngrok's
// certificate chain specifically (confirmed: httpbin.org HTTPS works fine,
// ngrok HTTPS gives httpCode = -1 every time) -- so we avoid TLS entirely.

const char* BACKEND_URL  = "http://hummus-gulf-unexpired.ngrok-free.dev";  // <-- put your new http:// ngrok URL here
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

//===================== BACKEND-DRIVEN STATE =====================
// This is the AUTHORITATIVE state whenever the backend is reachable.
// currentState (above) is only used as an OFFLINE FAIL-SAFE when the
// backend/WiFi is unreachable -- see loop() and updateOutputs().

bool backendReachable = false;
int backendState = SAFE_STATE;
bool backendActuate = false;

unsigned long lastSendTime = 0;
const unsigned long SEND_INTERVAL_MS = 1000;

// simple offline cache -- if a send fails, keep the reading and retry
// alongside the next one (Test Case 9b: zone caches readings and resyncs)
bool pendingRetry = false;

//===================== FIRE: DEBOUNCE + RECOVERY =====================
// A flicker shorter than FIRE_DEBOUNCE_MS never counts as real fire.
// Once confirmed, removing the flame does NOT snap risk back to 0 --
// it decays linearly over FIRE_RECOVERY_MS instead.
// NOTE: this local fireRisk/fireConfirmed is used for two things:
//   1. Local Serial debug + offline fail-safe actuation
//   2. NOT sent to the backend directly -- we send the RAW threshold
//      reading instead, so the BACKEND performs its own server-side
//      debounce (Test Case 6a: "server-side computation").

#define FIRE_RAW_THRESHOLD   1800
#define FIRE_DEBOUNCE_MS     1000
#define FIRE_RECOVERY_MS     3000

unsigned long fireAboveSince = 0;   // 0 = currently below threshold
bool fireConfirmed = false;
unsigned long fireDecayStart = 0;
float fireRisk = 0.0;               // 0.0 - 1.0 (local fail-safe use only)

//===================== GAS: WARM-UP =====================
// First 30s after boot, gas readings are ignored (sensor warm-up).
// FIX: this flag is now also sent to the backend so the backend doesn't
// escalate a zone off a gas reading that arrived during warm-up
// (Test Case 2d requires this to hold end-to-end, not just locally).

#define GAS_WARMUP_MS   30000
unsigned long bootTime = 0;
float gasRisk = 0.0;                // 0.0 - 1.0
bool gasWarmingUp = true;

//===================== WATER =====================

float waterRisk = 0.0;              // 0.0 - 1.0

//===================== PIR: DEBOUNCE + OFFLINE DETECTION =====================
// A brief exit+re-entry within ~2s must not spam separate events, so a new
// raw state has to hold for OCC_HOLD_MS before we trust it.
// A disconnected/floating PIR pin toggles erratically -- if it changes state
// more than PIR_TOGGLE_LIMIT times inside PIR_TOGGLE_WINDOW_MS, we treat the
// zone as OFFLINE instead of silently reporting "no motion".
// FIX: pirOffline is now sent to the backend (Test Case 4d requires the
// zone to show OFFLINE on the dashboard, not just on the local LED).

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
// risk_score = w_fire*fireRisk + w_gas*gasRisk + w_water*waterRisk + w_occ*occFactor
// weights sum to 100, so risk_score naturally falls in 0-100.
// Kept here ONLY for local Serial debug + offline fail-safe -- the backend
// computes its own copy of this from the raw values we send it, and THAT
// copy is what actually drives the dashboard and the priority ranking.

const float W_FIRE  = 40.0;
const float W_GAS   = 25.0;
const float W_WATER = 20.0;
const float W_OCC   = 15.0;

float riskScore = 0.0;

// LED blink timers
unsigned long previousMillis = 0;
bool ledState = LOW;
const long blinkInterval = 500;        // WARNING blink speed
const long offlineBlinkInterval = 150; // OFFLINE fast-blink speed

//===================== BUZZER: BEEP PATTERN (FIXED) =====================
// PROBLEM THIS FIXES: previously the buzzer was just digitalWrite(HIGH)
// once when entering CRITICAL and never toggled again -- so it played one
// continuous tone instead of an actual audible "beeping" alarm, and it
// never stopped beeping-in-a-loop the way a real siren would.
// FIX: the buzzer now toggles on/off on its own timer for as long as the
// zone stays in effectiveState == CRITICAL && effectiveActuate == true.
// It's driven independently of the LED blink timers so beep rate can be
// tuned separately from the red LED's solid-on behavior.

unsigned long buzzerPreviousMillis = 0;
bool buzzerState = LOW;
const long buzzerOnMs  = 150;   // beep duration
const long buzzerOffMs = 150;   // silence duration between beeps

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

  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(PIR_PIN, INPUT);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  bootTime = millis();
  pirToggleWindowStart = millis();

  Serial.println("======================================");
  Serial.println(" SMART CAMPUS SAFETY SYSTEM ");
  Serial.println("======================================");

  connectWifi();
  // no TLS setup needed -- BACKEND_URL is now a plain http:// ngrok tunnel
}

//===================== WIFI =====================

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
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
      fireDecayStart = 0;   // cancel any decay in progress
    }

  } else {

    fireAboveSince = 0;   // any "above" streak is broken

    if (fireConfirmed) {
      // was real fire, now removed -- decay gradually
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
      fireRisk = 0.0;   // just a brief flicker, never confirmed -- no decay needed
    }
  }
}

//===================== GAS: warm-up handling =====================

void processGas() {

  gasWarmingUp = (millis() - bootTime) < GAS_WARMUP_MS;

  if (gasWarmingUp) {
    gasRisk = 0.0;   // ignore readings during warm-up window
  } else {
    gasRisk = constrain((float)gasValue / 4095.0, 0.0, 1.0);
  }
}

//===================== WATER: normalize distance =====================

void processWater() {

  // distance == 999 means "no echo" -- treat as dry/safe
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

  // ---- toggle-rate check: a floating/disconnected pin toggles erratically ----
  if (raw != lastPirRaw) {

    if (now - pirToggleWindowStart > PIR_TOGGLE_WINDOW_MS) {
      pirToggleWindowStart = now;
      pirToggleCount = 0;
    }

    pirToggleCount++;
    lastPirRaw = raw;
  }

  pirOffline = (pirToggleCount > PIR_TOGGLE_LIMIT);

  // ---- debounce the occupancy state so brief exit/re-entry doesn't spam events ----
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
// IMPORTANT: we send ldrValue's RAW threshold check (not the locally
// debounced/decaying fireRisk) so the BACKEND performs its own server-side
// debounce -- this is what Test Case 6a is actually checking for.
//
// FIX 1: gas_warming_up is now included in the payload, so the backend can
// ignore/zero the gas contribution itself during warm-up instead of trusting
// a raw gasNorm value that could false-trigger a CRITICAL during boot.
//
// FIX 2: pir_offline is now included in the payload, so the backend (and
// therefore the dashboard) can mark this zone OFFLINE instead of silently
// treating a disconnected sensor as "occupancy = false".
//
// NOTE: switched from WiFiClientSecure to plain WiFiClient because Wokwi's
// simulated network fails the TLS handshake specifically against ngrok's
// certificate chain (confirmed via a side test: HTTPS to httpbin.org works,
// HTTPS to ngrok gives httpCode = -1 every time). BACKEND_URL must now point
// to a plain http:// ngrok tunnel (`ngrok http 8000 --scheme=http`).

void sendToBackend() {

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) {
      backendReachable = false;
      return;
    }
  }

  Serial.print("WiFi status before POST: ");
  Serial.println(WiFi.status());   // should print 3 (WL_CONNECTED)
  Serial.print("Free heap before POST: ");
  Serial.println(ESP.getFreeHeap());

  WiFiClient plainClient;   // no TLS needed for a plain http:// ngrok tunnel

  HTTPClient http;
  String url = String(BACKEND_URL) + "/api/ingest/" + ZONE_ID;

  http.begin(plainClient, url);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", ZONE_API_KEY);

  float rawFireSignal = (ldrValue > FIRE_RAW_THRESHOLD) ? 1.0 : 0.0;
  float gasNorm = constrain((float)gasValue / 4095.0, 0.0, 1.0);

  StaticJsonDocument<256> doc;
  doc["fire"] = rawFireSignal;
  doc["gas"] = gasNorm;
  doc["gas_warming_up"] = gasWarmingUp;          // FIX: backend must zero gas contribution while true
  doc["water"] = waterRisk;
  doc["occupancy"] = occupancyConfirmed;
  doc["pir_offline"] = pirOffline;               // FIX: backend must mark zone OFFLINE when true
  doc["sequence_no"] = (long)(millis());         // monotonic, survives resets better than a counter

  String body;
  serializeJson(doc, body);

  Serial.println("Sending POST to: " + url);

  int httpCode = http.POST(body);

  Serial.print("httpCode: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    String response = http.getString();
    Serial.print("Backend -> ");
    Serial.println(response);

    StaticJsonDocument<256> resp;
    if (deserializeJson(resp, response) == DeserializationError::Ok) {
      String stateStr = resp["state"] | "SAFE";
      backendActuate = resp["actuate"] | false;

      if (stateStr == "SAFE") backendState = SAFE_STATE;
      else if (stateStr == "WARNING") backendState = WARNING_STATE;
      else if (stateStr == "CRITICAL") backendState = CRITICAL_STATE;
      else if (stateStr == "OFFLINE") backendState = OFFLINE_STATE;

      backendReachable = true;
      pendingRetry = false;
    }
  } else {
    Serial.print("Backend send failed, httpCode=");
    Serial.println(httpCode);
    if (httpCode < 0) {
      Serial.println("  (negative code = network/TLS-level failure, not an HTTP response)");
    }
    backendReachable = false;
    pendingRetry = true;   // will simply retry on the next SEND_INTERVAL_MS tick
  }

  http.end();
}

//===================== PRINT SENSOR DATA =====================

void printSensorData() {

  Serial.println("--------------------------------------");

  Serial.print("LDR (fire) raw   : ");
  Serial.print(ldrValue);
  Serial.print("   fireRisk(local): ");
  Serial.println(fireRisk, 2);

  Serial.print("Gas raw          : ");
  Serial.print(gasValue);
  Serial.print("   gasRisk: ");
  Serial.print(gasRisk, 2);
  Serial.println(gasWarmingUp ? "   (WARMING UP - ignored)" : "");

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

  Serial.println("--------------------------------------");
}

//===================== CHECK STATUS (LOCAL fail-safe risk fusion) =====================
// This is only used when the backend is unreachable. When the backend IS
// reachable, backendState (set in sendToBackend/applyBackendResponse) is
// used instead -- see loop().

void checkStatus() {

  // PIR hardware fault takes priority -- report OFFLINE instead of a false SAFE
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

  // Use the BACKEND's decision whenever it's reachable (Test Case 5: the
  // backend sends a command back down to trigger buzzer/LED/relay).
  // Fall back to the local fail-safe decision only when offline.
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
      // FIX: relay still gates on effectiveActuate (a real cutoff should only
      // fire when the backend explicitly commands it), but the buzzer timer
      // below now runs independently every loop -- see updateBuzzer() call.
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
    updateBuzzer(false);   // FIX: explicitly silence buzzer on SAFE

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
    updateBuzzer(false);   // WARNING is visual-only, per spec: no buzzer/relay

    Serial.println("STATUS : WARNING");
  }

  //================ CRITICAL =================
  // FIX: this is the main bug fix. Previously the buzzer was set HIGH once
  // and left there -- effectively a single continuous tone, not a beep, and
  // it depended on stateChanged so it would only ever fire on the *entry*
  // transition into CRITICAL. Now updateBuzzer() runs every loop while the
  // zone remains CRITICAL, producing a real on/off beeping pattern for as
  // long as the hazard is active and the backend has authorized actuation.

  else if (effectiveState == CRITICAL_STATE) {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, HIGH);

    if (effectiveActuate) {
      digitalWrite(RELAY_PIN, HIGH);
      updateBuzzer(true);    // keeps toggling on/off every loop -- real beep
    } else {
      digitalWrite(RELAY_PIN, LOW);
      updateBuzzer(false);
    }

    Serial.println("STATUS : CRITICAL");
  }

  //================ OFFLINE =================
  // PIR sensor fault -- fast-blink RED, no buzzer/relay (a sensor fault
  // is not itself a confirmed hazard, so we don't trigger actuation).

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

  readSensors();

  processFire();
  processGas();
  processWater();
  processPir();

  checkStatus();   // local fail-safe estimate -- only used if backend is unreachable

  printSensorData();

  // Send raw readings to the backend on a fixed interval, and let the
  // backend's response drive the actual actuation (Test Case 6a, Test Case 5)
  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendToBackend();
  }

  updateOutputs();

  delay(200);   // finer sampling interval so debounce timing is accurate
}
