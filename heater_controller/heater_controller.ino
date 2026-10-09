// Heater enclosure controller - Arduino Uno R3
//
// Holds the enclosure at a steady temperature (default 500 F) using:
//   - one Type K thermocouple read through a MAX31855 or MAX6675 board
//   - PID control
//   - 6 heating elements switched by relays, staged so that the PID output
//     is spread across the elements (e.g. 50% power = 3 elements fully on)
//     and only ONE relay ever cycles to make up the fractional part.
//
// Libraries (Arduino IDE > Library Manager):
//   MAX31855 board: "Adafruit MAX31855 library"
//   MAX6675 board:  "MAX6675 library" by Adafruit
//
// Wiring (thermocouple board -> Uno):
//   VCC -> 5V   GND -> GND   SCK/CLK -> D13   SO/DO -> D12   CS -> D10
//   Thermocouple: yellow (+) and red (-) into the board's screw terminal.
// Relays: heater 1..6 relay inputs -> D2..D7, relay module VCC/GND -> 5V/GND.
//
// Serial Monitor / Serial Plotter at 9600 baud. Commands (type then Enter):
//   S520   set setpoint to 520 F       P3.0  set Kp
//   I0.02  set Ki                       D10   set Kd
//   ?      print current settings

#include <SPI.h>
#include <avr/wdt.h>

// ---------------- Thermocouple board ----------------
#define USE_MAX31855 1  // 1 = MAX31855 board, 0 = MAX6675 board

#if USE_MAX31855
#include "Adafruit_MAX31855.h"
#else
#include "max6675.h"
#endif

const uint8_t PIN_TC_CLK = 13;
const uint8_t PIN_TC_DO  = 12;
const uint8_t PIN_TC_CS  = 10;

#if USE_MAX31855
Adafruit_MAX31855 thermocouple(PIN_TC_CLK, PIN_TC_CS, PIN_TC_DO);
#else
MAX6675 thermocouple(PIN_TC_CLK, PIN_TC_CS, PIN_TC_DO);
#endif

// ---------------- Heaters / relays ----------------
const uint8_t NUM_HEATERS = 6;
const uint8_t RELAY_PINS[NUM_HEATERS] = {2, 3, 4, 5, 6, 7};
// Most common relay modules turn ON when the input is LOW. If your heaters
// come on when they should be off, set this to false.
const bool RELAY_ACTIVE_LOW = true;

// Time-proportioning window for the one cycling relay. Mechanical relays
// wear out with switching, so keep this long (10 s). With solid-state relays
// it can be shortened to ~2000 ms for even tighter control.
const unsigned long WINDOW_MS    = 10000;
const unsigned long MIN_PULSE_MS = 1000;  // ignore on/off pulses shorter than this

// ---------------- Control settings (Fahrenheit) ----------------
float setpointF = 500.0;
// Starting PID gains, output is in % of total heater power. These MUST be
// tuned on the real enclosure - see README.
float Kp = 3.0;    // % per F of error
float Ki = 0.02;   // % per (F * second)
float Kd = 0.0;    // % per (F / second)
const float INTEGRAL_BAND_F = 40.0;  // only integrate within +/- this of setpoint (prevents overshoot on warm-up)

// ---------------- Safety limits ----------------
const float MAX_SAFE_F = 560.0;      // over-temp: latch all heaters off
const uint8_t MAX_BAD_READS = 4;     // consecutive sensor faults (~1 s) before latching off
// Thermal runaway: if heaters are near full power for this long without the
// temperature rising at least this much, the thermocouple is probably loose
// or out of the enclosure - shut down.
const unsigned long RUNAWAY_TIME_MS = 10UL * 60UL * 1000UL;  // 10 minutes
const float RUNAWAY_MIN_RISE_F = 15.0;

// ---------------- Timing ----------------
const unsigned long READ_INTERVAL_MS = 250;   // MAX6675 needs >= 220 ms between reads
const unsigned long PID_INTERVAL_MS  = 1000;  // PID runs on the 1 s average of readings

// ---------------- State ----------------
float outputPct = 0;  // PID output, 0..100 % of total heater power
float integral = 0;
float lastTempF = NAN;
float currentTempF = NAN;
float readingSum = 0;
uint8_t readingCount = 0;
uint8_t badReads = 0;

bool faulted = false;
const __FlashStringHelper *faultReason = nullptr;

bool saturated = false;
unsigned long saturatedSinceMs = 0;
float saturatedStartTempF = 0;

unsigned long lastReadMs = 0;
unsigned long lastPidMs = 0;
unsigned long windowStartMs = 0;
uint8_t windowFullHeaters = 0;
unsigned long windowPartialMs = 0;

char cmdBuf[16];
uint8_t cmdLen = 0;

// ---------------------------------------------------------------

void writeRelay(uint8_t i, bool on) {
  digitalWrite(RELAY_PINS[i], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void allHeatersOff() {
  for (uint8_t i = 0; i < NUM_HEATERS; i++) writeRelay(i, false);
}

void tripFault(const __FlashStringHelper *reason) {
  if (!faulted) {
    faulted = true;
    faultReason = reason;
    Serial.print(F("FAULT: "));
    Serial.print(reason);
    Serial.println(F(" - all heaters latched OFF, reset the board to clear"));
  }
  outputPct = 0;
  allHeatersOff();
}

// Spread outputPct across the heaters: N fully on, one relay time-proportioned.
// The split is only recalculated at the start of each window so relays switch
// at most twice per window.
void applyHeaters(unsigned long now) {
  if (faulted) {
    allHeatersOff();
    return;
  }

  if (now - windowStartMs >= WINDOW_MS) {
    windowStartMs = now;
    float elements = outputPct / 100.0 * NUM_HEATERS;  // 0.0 .. 6.0
    windowFullHeaters = (uint8_t)elements;
    windowPartialMs = (unsigned long)((elements - windowFullHeaters) * WINDOW_MS);
    if (windowPartialMs < MIN_PULSE_MS) {
      windowPartialMs = 0;
    } else if (windowPartialMs > WINDOW_MS - MIN_PULSE_MS) {
      windowPartialMs = 0;
      windowFullHeaters++;
    }
  }

  unsigned long intoWindow = now - windowStartMs;
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    bool on = (i < windowFullHeaters) ||
              (i == windowFullHeaters && intoWindow < windowPartialMs);
    writeRelay(i, on);
  }
}

void runPid(float tempF, float dtS) {
  float error = setpointF - tempF;

  if (fabs(error) < INTEGRAL_BAND_F) {
    integral += Ki * error * dtS;
    integral = constrain(integral, 0.0, 100.0);
  }

  // Derivative on measurement avoids a kick when the setpoint changes
  float dInput = isnan(lastTempF) ? 0.0 : (tempF - lastTempF) / dtS;
  lastTempF = tempF;

  outputPct = constrain(Kp * error + integral - Kd * dInput, 0.0, 100.0);
}

void checkRunaway(unsigned long now, float tempF) {
  if (outputPct < 95.0) {
    saturated = false;
    return;
  }
  if (!saturated) {
    saturated = true;
    saturatedSinceMs = now;
    saturatedStartTempF = tempF;
  } else if (now - saturatedSinceMs >= RUNAWAY_TIME_MS) {
    if (tempF - saturatedStartTempF < RUNAWAY_MIN_RISE_F) {
      tripFault(F("thermal runaway (full power but temperature not rising)"));
    }
    saturatedSinceMs = now;
    saturatedStartTempF = tempF;
  }
}

void readSensor() {
  float tempF = thermocouple.readFahrenheit();

  if (isnan(tempF)) {
    Serial.print(F("Thermocouple fault"));
#if USE_MAX31855
    uint8_t fault = thermocouple.readError();
    if (fault & MAX31855_FAULT_OPEN)      Serial.print(F(" OPEN"));
    if (fault & MAX31855_FAULT_SHORT_GND) Serial.print(F(" SHORT_GND"));
    if (fault & MAX31855_FAULT_SHORT_VCC) Serial.print(F(" SHORT_VCC"));
#else
    Serial.print(F(" OPEN"));
#endif
    Serial.println();
    if (++badReads >= MAX_BAD_READS) tripFault(F("thermocouple not reading"));
    return;
  }

  badReads = 0;
  readingSum += tempF;
  readingCount++;
}

void printSettings() {
  Serial.print(F("Setpoint="));  Serial.print(setpointF, 1);
  Serial.print(F("F Kp="));      Serial.print(Kp, 3);
  Serial.print(F(" Ki="));       Serial.print(Ki, 4);
  Serial.print(F(" Kd="));       Serial.println(Kd, 3);
}

void runCommand(char *cmd) {
  char key = toupper(cmd[0]);
  if (key == '?') {
    printSettings();
    return;
  }
  if (cmd[1] == '\0') key = 0;  // no number given
  float v = atof(cmd + 1);

  switch (key) {
    case 'S':
      if (v > 0 && v <= MAX_SAFE_F - 25) setpointF = v;
      else Serial.println(F("Setpoint out of range"));
      break;
    case 'P': Kp = v; break;
    case 'I': Ki = v; break;
    case 'D': Kd = v; break;
    default:
      Serial.println(F("Commands: S<setpoint F>  P<kp>  I<ki>  D<kd>  ?"));
      return;
  }
  printSettings();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        runCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

void printStatus() {
  // label:value format works in both the Serial Monitor and Serial Plotter
  Serial.print(F("Temp_F:"));      Serial.print(currentTempF, 1);
  Serial.print(F(" Setpoint_F:")); Serial.print(setpointF, 1);
  Serial.print(F(" Output_pct:")); Serial.println(outputPct, 1);
}

// ---------------------------------------------------------------

void setup() {
  // Set relays to OFF before enabling them as outputs so nothing blips on at boot
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    writeRelay(i, false);
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelay(i, false);
  }

  Serial.begin(9600);
  delay(500);  // let the thermocouple board stabilize
#if USE_MAX31855
  if (!thermocouple.begin()) Serial.println(F("MAX31855 init failed"));
#endif
  Serial.println(F("Heater controller started"));
  printSettings();

  wdt_enable(WDTO_2S);  // reset (heaters off) if the code ever hangs
  windowStartMs = millis() - WINDOW_MS;  // start a fresh window immediately
}

void loop() {
  wdt_reset();
  unsigned long now = millis();

  handleSerial();

  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;
    readSensor();
  }

  if (now - lastPidMs >= PID_INTERVAL_MS) {
    float dtS = (now - lastPidMs) / 1000.0;
    lastPidMs = now;

    if (readingCount > 0) {
      currentTempF = readingSum / readingCount;
      readingSum = 0;
      readingCount = 0;

      if (currentTempF >= MAX_SAFE_F) tripFault(F("over-temperature"));
      if (!faulted) {
        runPid(currentTempF, dtS);
        checkRunaway(now, currentTempF);
      }
      printStatus();
    }
    if (faulted) {
      Serial.print(F("FAULTED: "));
      Serial.println(faultReason);
    }
  }

  applyHeaters(now);
}
