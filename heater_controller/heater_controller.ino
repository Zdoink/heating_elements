// Heater enclosure controller - Arduino Uno R3
//
// Holds the enclosure at a set temperature for a set time using:
//   - one Type K thermocouple read through a MAX31855 or MAX6675 board
//   - PID control
//   - 6 heating elements switched by relays, staged so that the PID output
//     is spread across the elements (e.g. 50% power = 3 elements fully on)
//     and only ONE relay ever cycles to make up the fractional part.
//   - a Nextion touch screen to set the temperature and time, start and stop
//
// Run sequence: IDLE -> (Start) -> HEATING -> reaches temp -> HOLDING (timer
// counts down) -> time up -> DONE. Heaters are only on in HEATING/HOLDING.
//
// Libraries (Arduino IDE > Library Manager):
//   "AltSoftSerial" by Paul Stoffregen
//   MAX31855 board: "Adafruit MAX31855 library"
//   MAX6675 board:  "MAX6675 library" by Adafruit
//
// Wiring:
//   Thermocouple board: VCC -> 5V, GND -> GND, SCK/CLK -> D13, SO/DO -> D12, CS -> D10
//   Relays: heater 1..6 relay inputs -> D2..D7
//   Nextion: TX (blue) -> D8, RX (yellow) -> D9, GND -> GND, 5V from its own supply
//
// USB Serial Monitor / Serial Plotter at 9600 baud. Commands (type then Enter):
//   S500  setpoint F     M90  hold time in minutes     G  start     X  stop
//   P3.0  set Kp         I0.02  set Ki                 D10  set Kd  ?  settings

#include <SPI.h>
#include <avr/wdt.h>
#include <AltSoftSerial.h>

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

// ---------------- Nextion screen ----------------
// AltSoftSerial is full duplex (unlike SoftwareSerial it can receive while
// sending), so screen commands are not lost. On the Uno it always uses
// D8 = RX (from Nextion TX) and D9 = TX (to Nextion RX).
AltSoftSerial nex;
const long NEX_BAUD = 9600;                 // Nextion factory default
const unsigned long NEX_UPDATE_MS = 1000;
// Screen frames carry a check number: <S500,29500>  value + check must equal this
const long FRAME_CHECK_TOTAL = 30000;

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

// ---------------- Run settings (Fahrenheit) ----------------
float setpointF = 500.0;
unsigned int durationMin = 60;            // hold time once at temperature
const float MIN_SETPOINT_F = 100.0;
const unsigned int MAX_DURATION_MIN = 1440;
const float READY_BAND_F = 5.0;           // hold timer starts within this of the setpoint

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
// Numbers are sent to the screen as aState, keep in sync with NEXTION.md
enum RunState : uint8_t { IDLE = 0, HEATING = 1, HOLDING = 2, DONE = 3, FAULT = 4 };
RunState state = IDLE;
unsigned long holdStartMs = 0;
const __FlashStringHelper *faultReason = nullptr;

float outputPct = 0;  // PID output, 0..100 % of total heater power
float integral = 0;
float lastTempF = NAN;
float currentTempF = NAN;
float readingSum = 0;
uint8_t readingCount = 0;
uint8_t badReads = 0;

bool saturated = false;
unsigned long saturatedSinceMs = 0;
float saturatedStartTempF = 0;

unsigned long lastReadMs = 0;
unsigned long lastPidMs = 0;
unsigned long lastNexMs = 0;
unsigned long windowStartMs = 0;
uint8_t windowFullHeaters = 0;
unsigned long windowPartialMs = 0;

char cmdBuf[16];
uint8_t cmdLen = 0;

char frameBuf[16];
uint8_t frameLen = 0;
bool inFrame = false;
bool nexUpdateNow = false;
unsigned int badFrames = 0;

// ---------------------------------------------------------------

bool isRunning() {
  return state == HEATING || state == HOLDING;
}

const __FlashStringHelper *stateName() {
  switch (state) {
    case IDLE:    return F("IDLE");
    case HEATING: return F("HEATING");
    case HOLDING: return F("HOLDING");
    case DONE:    return F("DONE");
    case FAULT:   return F("FAULT");
  }
  return F("?");
}

void writeRelay(uint8_t i, bool on) {
  digitalWrite(RELAY_PINS[i], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void allHeatersOff() {
  for (uint8_t i = 0; i < NUM_HEATERS; i++) writeRelay(i, false);
}

void tripFault(const __FlashStringHelper *reason) {
  if (state != FAULT) {
    state = FAULT;
    faultReason = reason;
    Serial.print(F("FAULT: "));
    Serial.print(reason);
    Serial.println(F(" - all heaters latched OFF, reset the board to clear"));
    nexUpdateNow = true;
  }
  outputPct = 0;
  allHeatersOff();
}

// Spread outputPct across the heaters: N fully on, one relay time-proportioned.
// The split is only recalculated at the start of each window so relays switch
// at most twice per window.
void applyHeaters(unsigned long now) {
  if (!isRunning()) {
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

// ---------------- Run control (shared by USB and the screen) ----------------

bool setSetpoint(float v) {
  if (v < MIN_SETPOINT_F || v > MAX_SAFE_F - 25) return false;
  setpointF = v;
  return true;
}

bool setDuration(long minutes) {
  if (minutes < 1 || minutes > (long)MAX_DURATION_MIN) return false;
  durationMin = minutes;
  return true;
}

// Repeated starts while already running are ignored, so the screen can
// safely resend Start without restarting the timer. A fault blocks starting.
void startRun() {
  if (state != IDLE && state != DONE) return;
  integral = 0;
  lastTempF = NAN;
  saturated = false;
  state = HEATING;
  if (!isnan(currentTempF)) runPid(currentTempF, PID_INTERVAL_MS / 1000.0);
  windowStartMs = millis() - WINDOW_MS;  // start a fresh relay window now
  Serial.println(F("Run started"));
}

void stopRun() {
  if (isRunning()) {
    state = IDLE;
    Serial.println(F("Run stopped"));
  }
  outputPct = 0;
  allHeatersOff();
}

void updateRunState(unsigned long now) {
  if (state == HEATING && currentTempF >= setpointF - READY_BAND_F) {
    state = HOLDING;
    holdStartMs = now;
    Serial.println(F("At temperature - hold timer started"));
  }
  if (state == HOLDING && now - holdStartMs >= durationMin * 60000UL) {
    state = DONE;
    outputPct = 0;
    allHeatersOff();
    Serial.println(F("Run complete - heaters off"));
  }
}

unsigned long secondsLeft(unsigned long now) {
  unsigned long total = durationMin * 60UL;
  if (state == HOLDING) {
    unsigned long elapsed = (now - holdStartMs) / 1000;
    return elapsed >= total ? 0 : total - elapsed;
  }
  if (state == DONE) return 0;
  return total;
}

// ---------------- USB Serial commands ----------------

void printSettings() {
  Serial.print(F("Setpoint="));  Serial.print(setpointF, 1);
  Serial.print(F("F Hold="));    Serial.print(durationMin);
  Serial.print(F("min Kp="));    Serial.print(Kp, 3);
  Serial.print(F(" Ki="));       Serial.print(Ki, 4);
  Serial.print(F(" Kd="));       Serial.print(Kd, 3);
  Serial.print(F(" State="));    Serial.print(stateName());
  Serial.print(F(" BadFrames=")); Serial.println(badFrames);
}

void runCommand(char *cmd) {
  char key = toupper(cmd[0]);
  switch (key) {
    case '?': printSettings(); return;
    case 'G': startRun(); nexUpdateNow = true; return;
    case 'X': stopRun();  nexUpdateNow = true; return;
  }

  if (cmd[1] == '\0') key = 0;  // no number given
  float v = atof(cmd + 1);

  switch (key) {
    case 'S':
      if (!setSetpoint(v)) Serial.println(F("Setpoint out of range"));
      break;
    case 'M':
      if (!setDuration((long)v)) Serial.println(F("Hold time out of range (1-1440 min)"));
      break;
    case 'P': Kp = v; break;
    case 'I': Ki = v; break;
    case 'D': Kd = v; break;
    default:
      Serial.println(F("Commands: S<setpoint F> M<minutes> G X P<kp> I<ki> D<kd> ?"));
      return;
  }
  nexUpdateNow = true;
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

// ---------------- Nextion: receiving ----------------
// Frames from the screen:
//   <S500,29500>  set setpoint    (value + check = 30000)
//   <M90,29910>   set hold time   (value + check = 30000)
//   <GO>          start
//   <X>           stop
// Anything outside < > (Nextion startup/status bytes, noise) is ignored, and
// a frame that is too long, has odd characters or a bad check is dropped.

void handleFrame(char *f) {
  bool ok = false;

  if (strcmp(f, "GO") == 0) {
    startRun();
    ok = true;
  } else if (strcmp(f, "X") == 0) {
    stopRun();
    ok = true;
  } else if (f[0] == 'S' || f[0] == 'M') {
    char *end;
    long value = strtol(f + 1, &end, 10);
    if (end != f + 1 && *end == ',') {
      char *checkStart = end + 1;
      long check = strtol(checkStart, &end, 10);
      if (end != checkStart && *end == '\0' && value + check == FRAME_CHECK_TOTAL) {
        ok = (f[0] == 'S') ? setSetpoint(value) : setDuration(value);
      }
    }
  }

  if (ok) {
    nexUpdateNow = true;  // echo the new values back right away
  } else {
    badFrames++;
    Serial.print(F("Rejected screen frame: "));
    Serial.println(f);
  }
}

void handleNextion() {
  while (nex.available()) {
    char c = nex.read();
    if (c == '<') {
      inFrame = true;
      frameLen = 0;
    } else if (!inFrame) {
      // ignore bytes between frames
    } else if (c == '>') {
      inFrame = false;
      frameBuf[frameLen] = '\0';
      handleFrame(frameBuf);
    } else if (frameLen < sizeof(frameBuf) - 1 && c >= ' ' && c <= '~') {
      frameBuf[frameLen++] = c;
    } else {
      inFrame = false;  // overflow or garbage: drop this frame
      badFrames++;
    }
  }
}

// ---------------- Nextion: sending ----------------
// Every Nextion instruction ends with three 0xFF bytes. The screen is fully
// refreshed every second, so a corrupted update is fixed on the next one.

void nexEnd() {
  nex.write(0xFF); nex.write(0xFF); nex.write(0xFF);
}

void nexVal(const __FlashStringHelper *obj, long v) {
  nex.print(obj); nex.print(F(".val=")); nex.print(v);
  nexEnd();
}

void nexTxtBegin(const __FlashStringHelper *obj) {
  nex.print(obj); nex.print(F(".txt=\""));
}

void nexTxtEnd() {
  nex.print('"');
  nexEnd();
}

void print2Digits(unsigned long v) {
  if (v < 10) nex.print('0');
  nex.print(v);
}

void updateNextion(unsigned long now) {
  nexTxtBegin(F("tTemp"));
  if (isnan(currentTempF)) nex.print(F("---"));
  else nex.print(currentTempF, 1);
  nexTxtEnd();

  nexTxtBegin(F("tStatus"));
  nex.print(stateName());
  nexTxtEnd();

  unsigned long s = secondsLeft(now);
  nexTxtBegin(F("tLeft"));
  nex.print(s / 3600); nex.print(':');
  print2Digits((s / 60) % 60); nex.print(':');
  print2Digits(s % 60);
  nexTxtEnd();

  nexTxtBegin(F("tOut"));
  nex.print((int)(outputPct + 0.5)); nex.print('%');
  nexTxtEnd();

  // Values the screen compares against to confirm its commands arrived
  nexVal(F("aSet"), (long)(setpointF + 0.5));
  nexVal(F("aMin"), durationMin);
  nexVal(F("aState"), state);
  nexVal(F("link"), 0);  // screen shows NO LINK if this isn't reset
}

// ---------------------------------------------------------------

void printStatus() {
  // label:value format works in both the Serial Monitor and Serial Plotter
  Serial.print(F("Temp_F:"));      Serial.print(currentTempF, 1);
  Serial.print(F(" Setpoint_F:")); Serial.print(setpointF, 1);
  Serial.print(F(" Output_pct:")); Serial.print(outputPct, 1);
  Serial.print(F(" State:"));      Serial.println(state);
}

void setup() {
  // Set relays to OFF before enabling them as outputs so nothing blips on at boot
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    writeRelay(i, false);
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelay(i, false);
  }

  Serial.begin(9600);
  nex.begin(NEX_BAUD);
  delay(500);  // let the thermocouple board and screen start up
#if USE_MAX31855
  if (!thermocouple.begin()) Serial.println(F("MAX31855 init failed"));
#endif
  nex.print(F("bkcmd=0"));  // stop the screen replying to every instruction
  nexEnd();

  Serial.println(F("Heater controller started - press Start (or send G) to run"));
  printSettings();

  wdt_enable(WDTO_2S);  // reset (heaters off) if the code ever hangs
}

void loop() {
  wdt_reset();
  unsigned long now = millis();

  handleSerial();
  handleNextion();

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
      if (isRunning()) {
        runPid(currentTempF, dtS);
        checkRunaway(now, currentTempF);
        updateRunState(now);
      }
      printStatus();
    }
    if (state == FAULT) {
      Serial.print(F("FAULTED: "));
      Serial.println(faultReason);
    }
  }

  if (nexUpdateNow || now - lastNexMs >= NEX_UPDATE_MS) {
    nexUpdateNow = false;
    lastNexMs = now;
    updateNextion(now);
  }

  applyHeaters(now);
}
