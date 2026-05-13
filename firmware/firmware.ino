/*
 * AERO-LITE v3.0
 *
 * Hardware used:
 *  - ESP32 S3 : Micro Controller
 *  - SCD41 (3) : CO2, Humidity, temperature recording sensors
 *  - TCA9548A  : I²C multiplexer
 *  
 * What this firmware does:
 *   - Reads CO₂, temperature, and humidity from THREE Sensirion SCD41 sensors
 *     (one at cabin inlet, one at purified outlet, one at CO₂ tank)
 *   - Cycles automatically through ADSORB → REGEN → PURGE states
 *   - Serves a live JSON API so the dashboard.html web UI can poll data
 *   - Logs state changes and sensor readings to a circular buffer
 * 
 * Architecture:
 *   - ESP32-S3 dual-core: Core 0 runs the sensor state machine (life-critical),
 *     Core 1 runs the WiFi web server (best-effort). This prevents network lag
 *     from delaying valve timing or heater control.
 *   - TCA9548A I²C multiplexer: All three SCD41 sensors share the same factory
 *     I²C address (0x62). The mux lets us switch between them on one bus.
 *   - FreeRTOS mutex: Protects shared sensor data so the web server and sensor
 *     task never read/write at the same time.
 * 
 * Libraries required:
 *   - Sensirion I2C SCD4x (official driver)
 *   - ESP32 Arduino core (built-in WiFi, WebServer, FreeRTOS)
 */

#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <SensirionI2cScd4x.h>

// ============================================================================
// WiFi Credentials — CHANGE THESE to match your network
// ============================================================================
#define WIFI_SSID       "<Wifi Network Name>"
#define WIFI_PASSWORD   "<Wifi Password>"

// ============================================================================
// Hardware Pin Map — ESP32-S3 DevKitC-1
// ============================================================================
// I²C bus for sensors and TCA9548A multiplexer
#define SDA_PIN         4          // I²C data line
#define SCL_PIN         5          // I²C clock line
#define TCA_ADDR        0x70       // TCA9548A multiplexer address

// Actuator / indicator pins (all switched via logic-level MOSFETs)
#define BLOWER_PIN      13         // 12V blower fan (ADSORB + PURGE)
#define VALVE_PIN       14         // 3-way solenoid valve (direction control)
#define PUMP_PIN        15         // 12V diaphragm pump/compressor (PURGE)
#define HEATER_LED      16         // LED simulates PTC heater status (Phase 1)
#define BLOWER_LED      17         // Visual feedback: blower is running
#define PUMP_LED        18         // Visual feedback: pump is running

// ============================================================================
// Timing & Buffer Configuration
// ============================================================================
#define READ_MS         500        // Sensor poll interval (ms). SCD41 updates every ~5 sec,
                                   // but we poll faster to catch new data immediately.
#define TELEM_MS        1000       // History push interval (ms). One data point per second.
#define HIST_SIZE       100        // Circular buffer: stores last 100 seconds of CO₂ data
#define STALE_MS        10000      // If a sensor hasn't reported in 10 sec, mark it stale
#define LOG_SIZE        20         // Circular log buffer: last 20 system events

// State machine timing — one full cycle = ~6 minutes (5 min + 1 min + 15 sec)
#define ADSORB_MS       300000     // 5 minutes: capture CO₂ in zeolite bed
#define REGEN_MS        60000      // 1 minute: heat bed to release concentrated CO₂
#define PURGE_MS        15000      // 15 seconds: sweep gas pushes CO₂ to buffer tank

// ============================================================================
// Fix for Sensirion library naming collision
// ============================================================================
// The SCD4x library defines NO_ERROR as a macro. We undefine it first to avoid
// conflicts with ESP32 core headers, then redefine it for our own use.
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// ============================================================================
// Objects
// ============================================================================
// One SCD41 object per sensor. All three share the same Wire bus; the mux
// selects which one is active before each read/write.
SensirionI2cScd4x scd0;   // Ch0: Cabin inlet (raw cabin air)
SensirionI2cScd4x scd1;   // Ch1: Purified outlet (after zeolite bed)
SensirionI2cScd4x scd2;   // Ch2: CO₂ Tank (concentrated output)

// Web server on port 80. Serves JSON endpoints for the dashboard.
WebServer server(80);

// System operating modes. The state machine cycles automatically.
enum State { ST_ADSORB, ST_REGEN, ST_PURGE };
volatile State curState = ST_ADSORB;      // Current mode (volatile: shared across tasks)
volatile unsigned long stateStart = 0;    // millis() timestamp when current mode began

// ============================================================================
// Data Structures
// ============================================================================

// Latest snapshot from all three sensors. Updated by sensorTask, read by HTTP handlers.
struct SensorData {
    float co2_1, co2_2, co2_3;            // CO₂ in ppm
    float rh_1, rh_2, rh_3;               // Relative humidity in %
    float temp_1, temp_2, temp_3;         // Temperature in °C (raw from sensor)
    bool ok_1, ok_2, ok_3;                // true = fresh data within STALE_MS window
    unsigned long ms_1, ms_2, ms_3;       // millis() timestamp of last successful read
};

// One history point for the live chart. Stores CO₂ from all 3 channels + state.
struct HistPt {
    unsigned long t;                      // Timestamp (ms since boot)
    float co2_1, co2_2, co2_3;            // CO₂ readings at this moment
    State st;                             // Which state the system was in
};

// One log entry for the diagnostics console.
struct LogEntry {
    unsigned long ms;                     // Timestamp
    char msg[64];                         // Human-readable message
};

// ============================================================================
// Global Variables
// ============================================================================

// Shared sensor data. Protected by mtx — NEVER touch without locking!
SensorData latest = {0,0,0,0,0,0,0,0,0,false,false,false,0,0,0};

// Circular history buffer for the live chart. Overwrites old data when full.
HistPt hist[HIST_SIZE];
int histHead = 0;     // Next write position
int histCount = 0;    // How many valid points stored (caps at HIST_SIZE)

// Circular log buffer for system events (state changes, errors, etc.).
LogEntry logs[LOG_SIZE];
int logHead = 0;
int logCount = 0;

// FreeRTOS mutex (semaphore). Prevents the web server (Core 1) from reading
// sensor data while sensorTask (Core 0) is writing it.
SemaphoreHandle_t mtx;

// Reusable error buffer for Sensirion library calls
static char errMsg[64];
static int16_t err;

// ============================================================================
// Function Prototypes
// ============================================================================
void tcaSelect(uint8_t ch);
bool initScd41(SensirionI2cScd4x& s, uint8_t ch, const char* name);
void addLog(const String& msg);
void readSensors();
void pushHist();
void enterState(State s);
void sensorTask(void* pv);
void sendJson(int code, const String& payload);
void handleTelem();
void handleHist();
void handleSet();
void handleLogs();
String escJson(const char* s);

// ============================================================================
// Setup — Runs once on boot
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[ AERO-LITE v3.0 ] Boot");

    // Initialize all actuator pins to OFF (LOW) for safety
    pinMode(BLOWER_PIN, OUTPUT); digitalWrite(BLOWER_PIN, LOW);
    pinMode(VALVE_PIN,  OUTPUT); digitalWrite(VALVE_PIN,  LOW);
    pinMode(PUMP_PIN,   OUTPUT); digitalWrite(PUMP_PIN,   LOW);
    pinMode(HEATER_LED, OUTPUT); digitalWrite(HEATER_LED, LOW);
    pinMode(BLOWER_LED, OUTPUT); digitalWrite(BLOWER_LED, LOW);
    pinMode(PUMP_LED,   OUTPUT); digitalWrite(PUMP_LED,   LOW);

    // Start I²C at 100 kHz (standard mode). SCD41 is reliable at this speed.
    Wire.begin(SDA_PIN, SCL_PIN, 100000);

    // Enable internal pull-ups on SDA/SCL. Helps signal integrity with short wires
    // and reduces the need for external 4.7kΩ resistors on the breadboard.
    gpio_pullup_en((gpio_num_t)SDA_PIN);
    gpio_pullup_en((gpio_num_t)SCL_PIN);

    // Verify the TCA9548A multiplexer is responding before initializing sensors
    Wire.beginTransmission(TCA_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[ERR] TCA9548A not found at 0x70");
        while (true) { delay(1000); }   // Halt — no point continuing without mux
    }
    Serial.println("[OK]  TCA9548A detected");

    // Initialize each SCD41 on its mux channel. If any fails, we log it but
    // continue — the dashboard will show a fault instead of crashing.
    initScd41(scd0, 0, "Cabin");
    initScd41(scd1, 1, "Purified");
    initScd41(scd2, 2, "CO2 Tank");

    // Connect to WiFi in Station mode (join existing network). Max TX power
    // for best range since the prototype may sit across the room from the router.
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500); Serial.print("."); attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[ERR] WiFi failed. Check credentials.");
        while (true) { delay(1000); }   // Halt — dashboard can't work without network
    }
    Serial.print("\n[OK]  IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("[INFO] Use this IP in dashboard.html");

    // Register HTTP endpoints. All return JSON and support CORS so the dashboard
    // can be opened from any device on the same network.
    server.on("/api/telemetry", handleTelem);   // Current sensor snapshot
    server.on("/api/history",   handleHist);    // Last 100 seconds of chart data
    server.on("/api/setstate",  HTTP_POST, handleSet);  // Manual override (for testing)
    server.on("/api/logs",      handleLogs);    // System event log
    server.begin();
    Serial.println("[OK]  JSON API on port 80");

    // Create the mutex and launch the sensor task on Core 0 (life-critical).
    // Core 1 is left free for the WiFi stack and web server loop().
    mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(sensorTask, "sensorTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[OK]  Running.\n");
}

// ============================================================================
// Loop — Runs on Core 1 (WiFi / Web Server)
// ============================================================================
void loop() {
    server.handleClient();          // Process incoming HTTP requests
    vTaskDelay(pdMS_TO_TICKS(5));   // Yield briefly so FreeRTOS can schedule
}

// ============================================================================
// Multiplexer Control
// ============================================================================
// Selects one of 8 channels on the TCA9548A. Only channels 0–2 are used.
// Call this before EVERY read or write to a sensor so the bus talks to the
// right device.
void tcaSelect(uint8_t ch) {
    if (ch > 7) return;
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(1 << ch);            // Channel mask: 0x01, 0x02, 0x04, etc.
    Wire.endTransmission();
}

// ============================================================================
// SCD41 Sensor Initialization
// ============================================================================
// Wakes the sensor, stops any previous measurement, reinitializes registers,
// reads the serial number to confirm communication, then starts periodic
// measurement mode (one reading every ~5 seconds).
bool initScd41(SensirionI2cScd4x& s, uint8_t ch, const char* name) {
    Serial.printf("[INIT] Ch%d — %s\n", ch, name);
    tcaSelect(ch);
    s.begin(Wire, SCD41_I2C_ADDR_62);
    delay(30);
    s.wakeUp(); delay(10);
    s.stopPeriodicMeasurement(); delay(500);   // Clear any previous state
    s.reinit(); delay(10);                     // Soft reset to known defaults

    uint64_t serial = 0;
    err = s.getSerialNumber(serial);
    if (err != NO_ERROR) {
        errorToString(err, errMsg, sizeof(errMsg));
        addLog("[INIT FAIL] Ch" + String(ch) + " " + String(name) + ": " + String(errMsg));
        Serial.printf("[ERR] Ch%d not found\n", ch);
        return false;
    }
    Serial.printf("[OK]  Ch%d %s serial: 0x%08X%08X\n", ch, name,
                  (uint32_t)(serial >> 32), (uint32_t)(serial & 0xFFFFFFFF));
    s.startPeriodicMeasurement();
    Serial.printf("[OK]  Ch%d %s running\n", ch, name);
    return true;
}

// ============================================================================
// Logging
// ============================================================================
// Adds a message to the circular log buffer. Thread-safe: grabs the mutex
// so this can be called from sensorTask without colliding with handleLogs().
void addLog(const String& msg) {
    String line = msg;
    if (line.length() > 63) line = line.substring(0, 63);   // Truncate to fit buffer
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        logs[logHead].ms = millis();
        strncpy(logs[logHead].msg, line.c_str(), 63);
        logs[logHead].msg[63] = '\0';   // Ensure null termination
        logHead = (logHead + 1) % LOG_SIZE;
        if (logCount < LOG_SIZE) logCount++;
        xSemaphoreGive(mtx);
    }
    Serial.println(line);   // Also echo to Serial for debugging
}

// ============================================================================
// JSON String Escaping
// ============================================================================
// Escapes quotes, backslashes, and newlines so user messages don't break the
// JSON structure when served via /api/logs. Critical for safety.
String escJson(const char* s) {
    String out = "";
    for (int i = 0; s[i] && i < 64; i++) {
        if (s[i] == '"') out += "\\\"";
        else if (s[i] == '\\') out += "\\\\";
        else if (s[i] == '\n') out += "\\n";
        else out += s[i];
    }
    return out;
}

// ============================================================================
// Sensor Task — Runs on Core 0 (FreeRTOS)
// ============================================================================
// This is the heart of the system. It polls sensors every READ_MS, manages the
// automatic ADSORB → REGEN → PURGE state transitions, and pushes telemetry
// to the history buffer. Runs forever in its own task.
void sensorTask(void* pv) {
    unsigned long lastRead = 0, lastHist = 0;
    enterState(ST_ADSORB);   // Start in capture mode
    for (;;) {
        unsigned long now = millis();

        // Poll sensors at fixed interval
        if (now - lastRead >= READ_MS) { lastRead = now; readSensors(); }

        // Automatic state machine: check elapsed time and advance to next state
        unsigned long elapsed = now - stateStart;
        switch (curState) {
            case ST_ADSORB: if (elapsed >= ADSORB_MS) enterState(ST_REGEN); break;
            case ST_REGEN:  if (elapsed >= REGEN_MS)  enterState(ST_PURGE); break;
            case ST_PURGE:  if (elapsed >= PURGE_MS)  enterState(ST_ADSORB); break;
        }

        // Push a history point every TELEM_MS for the live chart
        if (now - lastHist >= TELEM_MS) { lastHist = now; pushHist(); }

        vTaskDelay(pdMS_TO_TICKS(100));   // 10 Hz task loop
    }
}

// ============================================================================
// State Transition
// ============================================================================
// Safely switches operating modes. ALWAYS resets all actuators to OFF first,
// then enables only the ones needed for the new state. This prevents dangerous
// overlaps (e.g., blower + heater simultaneously).
void enterState(State s) {
    curState = s;
    stateStart = millis();

    // EMERGENCY-STOP: Turn everything OFF before configuring new state
    digitalWrite(BLOWER_PIN, LOW);
    digitalWrite(VALVE_PIN,  LOW);
    digitalWrite(PUMP_PIN,   LOW);
    digitalWrite(HEATER_LED, LOW);
    digitalWrite(BLOWER_LED, LOW);
    digitalWrite(PUMP_LED,   LOW);

    switch (s) {
        case ST_ADSORB:
            // ADSORB: Pull cabin air through dehumidifier and zeolite bed.
            // Clean, CO₂-depleted air returns to the habitat.
            digitalWrite(BLOWER_PIN, HIGH);
            digitalWrite(BLOWER_LED, HIGH);
            addLog("[STATE] ADSORB");
            break;

        case ST_REGEN:
            // REGEN: Seal the chamber and heat the zeolite bed.
            // CO₂ desorbs into the sealed regeneration chamber.
            // Blower is OFF — cabin air loop is fully isolated for safety.
            digitalWrite(HEATER_LED, HIGH);   // LED simulates heater (Phase 1)
            addLog("[STATE] REGEN");
            break;

        case ST_PURGE:
            // PURGE: Use sweep gas to push concentrated CO₂ out of the bed,
            // through the compressor, and into the buffer tank.
            digitalWrite(BLOWER_PIN, HIGH);   // Sweep gas ON
            digitalWrite(VALVE_PIN,  HIGH);   // Route to tank
            digitalWrite(PUMP_PIN,   HIGH);   // Compressor ON
            digitalWrite(BLOWER_LED, HIGH);
            digitalWrite(PUMP_LED,   HIGH);
            addLog("[STATE] PURGE");
            break;
    }
}

// ============================================================================
// Sensor Reading
// ============================================================================
// Reads all three SCD41 sensors via the multiplexer. Each sensor is checked for
// fresh data (dataReadyStatus). If new data exists, we read it; if not, we skip
// and rely on the stale-timeout logic to mark it later.
//
// The mutex is held ONLY during the brief update to `latest` — never during the
// slow I²C transactions — so the web server stays responsive.
void readSensors() {
    // Temporary structs to hold readings from each channel
    struct R { uint16_t co2; float t, rh; bool got; } r0={0,0,0,false}, r1={0,0,0,false}, r2={0,0,0,false};
    bool ready;

    // --- Channel 0: Cabin Inlet ---
    tcaSelect(0);
    ready = false;
    err = scd0.getDataReadyStatus(ready);
    if (err == NO_ERROR && ready) {
        err = scd0.readMeasurement(r0.co2, r0.t, r0.rh);
        if (err == NO_ERROR && r0.co2 != 0) r0.got = true;   // 0 ppm = invalid / not ready
    }

    // --- Channel 1: Purified Outlet ---
    tcaSelect(1);
    ready = false;
    err = scd1.getDataReadyStatus(ready);
    if (err == NO_ERROR && ready) {
        err = scd1.readMeasurement(r1.co2, r1.t, r1.rh);
        if (err == NO_ERROR && r1.co2 != 0) r1.got = true;
    }

    // --- Channel 2: CO₂ Tank ---
    tcaSelect(2);
    ready = false;
    err = scd2.getDataReadyStatus(ready);
    if (err == NO_ERROR && ready) {
        err = scd2.readMeasurement(r2.co2, r2.t, r2.rh);
        if (err == NO_ERROR && r2.co2 != 0) r2.got = true;
    }

    // Atomically update the shared `latest` struct under mutex protection
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        unsigned long now = millis();

        // Update Cabin sensor if we got fresh data; otherwise keep last known value
        if (r0.got) {
            latest.co2_1 = r0.co2;
            latest.temp_1 = r0.t;
            latest.rh_1 = r0.rh;
            latest.ms_1 = now;
        }
        // Sensor is "OK" if we just got data OR if last data is within stale window
        latest.ok_1 = r0.got || (now - latest.ms_1 < STALE_MS);

        if (r1.got) {
            latest.co2_2 = r1.co2;
            latest.temp_2 = r1.t;
            latest.rh_2 = r1.rh;
            latest.ms_2 = now;
        }
        latest.ok_2 = r1.got || (now - latest.ms_2 < STALE_MS);

        if (r2.got) {
            latest.co2_3 = r2.co2;
            latest.temp_3 = r2.t;
            latest.rh_3 = r2.rh;
            latest.ms_3 = now;
        }
        latest.ok_3 = r2.got || (now - latest.ms_3 < STALE_MS);

        xSemaphoreGive(mtx);
    }
}

// ============================================================================
// History Buffer Push
// ============================================================================
// Stores one snapshot of all three CO₂ channels + current state into the circular
// buffer. The dashboard polls /api/history to draw the live line chart.
void pushHist() {
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        HistPt pt = { millis(), latest.co2_1, latest.co2_2, latest.co2_3, curState };
        hist[histHead] = pt;
        histHead = (histHead + 1) % HIST_SIZE;
        if (histCount < HIST_SIZE) histCount++;
        xSemaphoreGive(mtx);
    }
}

// ============================================================================
// JSON API Helpers & Handlers
// ============================================================================

// Send JSON response with CORS headers so the web dashboard (served from any
// local file or device) can fetch data without browser security errors.
void sendJson(int code, const String& payload) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(code, "application/json", payload);
}

// --- GET /api/telemetry ---
// Returns the current sensor snapshot, system state, and sensor health flags.
// This is polled by the dashboard every second.
void handleTelem() {
    String json = "{";
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        unsigned long now = millis();
        float tempF = latest.temp_1 * 9.0f / 5.0f + 32.0f;   // Convert °C → °F for display

        json += "\"co2_1\":" + String(latest.co2_1, 0) + ",";
        json += "\"co2_2\":" + String(latest.co2_2, 0) + ",";
        json += "\"co2_3\":" + String(latest.co2_3, 0) + ",";
        json += "\"rh_1\":" + String(latest.rh_1, 1) + ",";
        json += "\"rh_2\":" + String(latest.rh_2, 1) + ",";
        json += "\"temp_f\":" + String(tempF, 1) + ",";
        json += "\"state\":\"" + String(curState == ST_ADSORB ? "ADSORB" : curState == ST_REGEN ? "REGEN" : "PURGE") + "\",";
        json += "\"state_sec\":" + String((now - stateStart) / 1000) + ",";
        json += "\"ok_1\":" + String(latest.ok_1 ? "true" : "false") + ",";
        json += "\"ok_2\":" + String(latest.ok_2 ? "true" : "false") + ",";
        json += "\"ok_3\":" + String(latest.ok_3 ? "true" : "false") + ",";
        // Age = seconds since last successful read (helps diagnose stale sensors)
        json += "\"age_1\":" + String((now - latest.ms_1) / 1000.0f, 1) + ",";
        json += "\"age_2\":" + String((now - latest.ms_2) / 1000.0f, 1) + ",";
        json += "\"age_3\":" + String((now - latest.ms_3) / 1000.0f, 1);
        xSemaphoreGive(mtx);
    }
    json += "}";
    sendJson(200, json);
}

// --- GET /api/history ---
// Returns the last HIST_SIZE seconds of data as an array for the live chart.
void handleHist() {
    String json = "{\"points\":[";
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        int count = min(histCount, HIST_SIZE);
        int idx = (histHead - count + HIST_SIZE) % HIST_SIZE;   // Oldest valid index
        for (int i = 0; i < count; i++) {
            if (i > 0) json += ",";
            json += "{\"t\":" + String(hist[idx].t)
                 + ",\"c1\":" + String(hist[idx].co2_1, 1)
                 + ",\"c2\":" + String(hist[idx].co2_2, 1)
                 + ",\"c3\":" + String(hist[idx].co2_3, 1)
                 + ",\"st\":" + String(hist[idx].st) + "}";
            idx = (idx + 1) % HIST_SIZE;
        }
        xSemaphoreGive(mtx);
    }
    json += "]}";
    sendJson(200, json);
}

// --- GET /api/logs ---
// Returns the last LOG_SIZE system events (state changes, init results, errors).
void handleLogs() {
    String json = "{\"logs\":[";
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        int count = min(logCount, LOG_SIZE);
        int idx = (logHead - count + LOG_SIZE) % LOG_SIZE;
        for (int i = 0; i < count; i++) {
            if (i > 0) json += ",";
            json += "{\"ms\":" + String(logs[idx].ms)
                 + ",\"msg\":\"" + escJson(logs[idx].msg) + "\"}";
            idx = (idx + 1) % LOG_SIZE;
        }
        xSemaphoreGive(mtx);
    }
    json += "]}";
    sendJson(200, json);
}

// --- POST /api/setstate ---
// Manual override for testing. Accepts ?state=ADSORB, REGEN, or PURGE.
// Use this to force a state during bench testing without waiting for timers.
void handleSet() {
    String s = server.arg("state");
    if (s == "ADSORB") enterState(ST_ADSORB);
    else if (s == "REGEN") enterState(ST_REGEN);
    else if (s == "PURGE") enterState(ST_PURGE);
    sendJson(200, "{\"ok\":true}");
}