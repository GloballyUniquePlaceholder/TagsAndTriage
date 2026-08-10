// THIS IS ALL CLAUDE


/*
 * Pico WH BLE Heart Rate Monitor
 * ------------------------------
 * Reads a KY-039 PPG (photoplethysmography) sensor on GPIO26 (ADC0),
 * finds heartbeats in the signal, and exposes the result over the
 * standard Bluetooth GATT Heart Rate Service (0x180D) so any BLE
 * central (phone, watch, or the companion Python script) can subscribe
 * to notifications.
 *
 * Hardware:
 *   KY-039  VCC -> 3V3   (NOT 5V -- RP2040 ADC pins are not 5V tolerant)
 *   KY-039  GND -> GND
 *   KY-039  S   -> GPIO26 / ADC0
 *
 * Framework: Arduino-Pico core (earlephilhower) via PlatformIO, BLE.h library.
 */

#include <Arduino.h>
#include <BLE.h>
#include <math.h>

// ---------------------------------------------------------------------------
// BLE GATT objects
// ---------------------------------------------------------------------------
static BLEService *hrService;
static BLECharacteristic *hrMeasurementChar;   // 0x2A37 - Heart Rate Measurement
static BLECharacteristic *bodySensorLocChar;   // 0x2A38 - Body Sensor Location

static const char *DEVICE_NAME = "PicoW-HRM";

// ---------------------------------------------------------------------------
// Sensor + peak-detection configuration
// ---------------------------------------------------------------------------
static const int SENSOR_PIN = 26;              // ADC0
static const uint32_t SAMPLE_INTERVAL_US = 4000;   // 250 Hz sampling
static const uint32_t NOTIFY_INTERVAL_MS = 1000;   // publish over BLE 1x/sec

// Fast smoothing (kills ADC/electrical noise, keeps the pulse waveform)
static const float SMOOTH_ALPHA = 0.35f;

// Slow baseline tracking (removes DC drift / ambient light changes)
static const float BASELINE_ALPHA = 0.015f;

// Adaptive amplitude tracking used to size the beat-detection threshold
static const float AMPLITUDE_ALPHA = 0.02f;
static const float THRESHOLD_FRACTION = 0.5f;  // trigger at 50% of tracked amplitude
static const float MIN_THRESHOLD = 3.0f;       // floor so pure noise can't trigger

// Physiological bounds used to reject implausible beats (30-220 bpm)
static const uint32_t MIN_IBI_MS = 273;   // 220 bpm
static const uint32_t MAX_IBI_MS = 2000;  // 30 bpm
static const uint32_t REFRACTORY_MS = 300; // hard minimum spacing between beats

static const int IBI_HISTORY_LEN = 4;     // beats averaged for a stable BPM
static const uint32_t CONTACT_TIMEOUT_MS = 3000; // no beat this long -> "no contact"

// Set to 1 to print "raw,smoothed,acSignal,threshold" over Serial for tuning
// with PlatformIO's / Arduino IDE's Serial Plotter.
#define DEBUG_SERIAL_PLOT 1

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static float smoothed = 0.0f;
static float baseline = 0.0f;
static float signalAmplitude = 50.0f;
static bool baselineInitialized = false;

static bool aboveThreshold = false;
static bool haveLastBeat = false;
static uint32_t lastBeatMillis = 0;
static uint32_t lastValidBeatMillis = 0;

static uint32_t ibiHistory[IBI_HISTORY_LEN];
static int ibiIndex = 0;
static int ibiCount = 0;

static uint8_t currentBPM = 0;
static bool contactDetected = false;

// ---------------------------------------------------------------------------
// Sample the sensor, run the peak detector, update currentBPM/contactDetected
// ---------------------------------------------------------------------------
static void sampleSensor() {
    int raw = analogRead(SENSOR_PIN); // 0-4095 (12-bit resolution set in setup)

    if (!baselineInitialized) {
        smoothed = (float)raw;
        baseline = (float)raw;
        baselineInitialized = true;
        return;
    }

    // Fast EMA smooths sample-to-sample noise while keeping the pulse shape
    smoothed += SMOOTH_ALPHA * ((float)raw - smoothed);

    // Slow EMA tracks the DC baseline (ambient light, finger pressure drift)
    baseline += BASELINE_ALPHA * (smoothed - baseline);

    float acSignal = smoothed - baseline; // AC-coupled PPG waveform

    // Track a running amplitude estimate to size an adaptive threshold
    float rectified = fabsf(acSignal);
    signalAmplitude += AMPLITUDE_ALPHA * (rectified - signalAmplitude);

    float threshold = THRESHOLD_FRACTION * signalAmplitude;
    if (threshold < MIN_THRESHOLD) threshold = MIN_THRESHOLD;

    uint32_t now = millis();

#if DEBUG_SERIAL_PLOT
    static uint32_t lastPlot = 0;
    if (now - lastPlot >= 20) { // ~50 Hz plot rate is plenty for the eye
        lastPlot = now;
        Serial.print(raw);
        Serial.print(',');
        Serial.print(acSignal);
        Serial.print(',');
        Serial.println(threshold);
    }
#endif

    if (!aboveThreshold && acSignal > threshold) {
        // Rising edge through the threshold = candidate heartbeat
        if (now - lastBeatMillis > REFRACTORY_MS) {
            if (!haveLastBeat) {
                // First edge ever seen: just set the reference point
                haveLastBeat = true;
            } else {
                uint32_t ibi = now - lastBeatMillis;
                if (ibi >= MIN_IBI_MS && ibi <= MAX_IBI_MS) {
                    ibiHistory[ibiIndex] = ibi;
                    ibiIndex = (ibiIndex + 1) % IBI_HISTORY_LEN;
                    if (ibiCount < IBI_HISTORY_LEN) ibiCount++;

                    uint32_t sum = 0;
                    for (int i = 0; i < ibiCount; i++) sum += ibiHistory[i];
                    uint32_t avgIbi = sum / ibiCount;

                    uint32_t bpmCalc = 60000UL / avgIbi;
                    if (bpmCalc < 30) bpmCalc = 30;
                    if (bpmCalc > 220) bpmCalc = 220;
                    currentBPM = (uint8_t)bpmCalc;

                    lastValidBeatMillis = now;
                    contactDetected = true;

                    digitalWrite(LED_BUILTIN, HIGH); // brief flash on each beat
                }
            }
            lastBeatMillis = now;
        }
        aboveThreshold = true;
    } else if (aboveThreshold && acSignal < threshold * 0.5f) {
        aboveThreshold = false;
        digitalWrite(LED_BUILTIN, LOW);
    }

    // No valid beat for a while -> report "no contact" and forget stale history
    if (now - lastValidBeatMillis > CONTACT_TIMEOUT_MS) {
        if (contactDetected) {
            contactDetected = false;
            ibiCount = 0;
            ibiIndex = 0;
            currentBPM = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Build and push the standard Heart Rate Measurement payload
// ---------------------------------------------------------------------------
static void publishHeartRate() {
    // Flags byte (Bluetooth GATT Heart Rate Measurement spec):
    //   bit0 = 0        -> HR value is UINT8
    //   bit1 = 1        -> sensor contact feature supported
    //   bit2 = contact  -> sensor contact detected
    //   bit3 = 0        -> no energy expended field
    //   bit4 = 0        -> no RR-interval field
    uint8_t flags = (1 << 1);
    if (contactDetected) flags |= (1 << 2);

    uint8_t payload[2];
    payload[0] = flags;
    payload[1] = contactDetected ? currentBPM : 0;

    hrMeasurementChar->setValue(payload, sizeof(payload));
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    analogReadResolution(12); // use the RP2040's full 12-bit ADC range (0-4095)

    BLE.begin(DEVICE_NAME);

    BLEServer *server = BLE.server();

    hrService = new BLEService(BLEUUID((uint16_t)0x180D)); // Heart Rate Service

    hrMeasurementChar = new BLECharacteristic(
        BLEUUID((uint16_t)0x2A37),
        BLERead | BLENotify,
        "Heart Rate Measurement");

    bodySensorLocChar = new BLECharacteristic(
        BLEUUID((uint16_t)0x2A38),
        BLERead,
        "Body Sensor Location");

    hrService->addCharacteristic(hrMeasurementChar);
    hrService->addCharacteristic(bodySensorLocChar);
    server->addService(hrService);

    bodySensorLocChar->setValue((uint8_t)3); // 3 = "Finger"

    uint8_t initPayload[2] = {(uint8_t)(1 << 1), 0}; // contact-not-detected, 0 bpm
    hrMeasurementChar->setValue(initPayload, sizeof(initPayload));

    BLE.startAdvertising(true);

    Serial.println("BLE Heart Rate Service advertising as \"PicoW-HRM\"");
#if DEBUG_SERIAL_PLOT
    Serial.println("raw,acSignal,threshold");
#endif
}

// ---------------------------------------------------------------------------
void loop() {
    static uint32_t lastSampleUs = 0;
    static uint32_t lastNotifyMs = 0;

    uint32_t nowUs = micros();
    if (nowUs - lastSampleUs >= SAMPLE_INTERVAL_US) {
        lastSampleUs = nowUs;
        sampleSensor();
    }

    uint32_t nowMs = millis();
    if (nowMs - lastNotifyMs >= NOTIFY_INTERVAL_MS) {
        lastNotifyMs = nowMs;
        publishHeartRate();
    }
}