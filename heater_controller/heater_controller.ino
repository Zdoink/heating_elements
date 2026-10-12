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
//
// ============================================================================
// HOW TO READ THE COMMENTS
//   Plain comments explain what the code does for the heater.
//   Comments starting with "C++:" explain the language feature being used,
//   so you can learn C++ as you read.
//
// C++: Anything after // to the end of the line is a comment. The compiler
//      ignores it completely, so comments never change what the program does.
//
// C++: An Arduino sketch is C++. Behind the scenes the Arduino core supplies a
//      main() function that calls setup() once, then calls loop() over and
//      over forever. Those two functions are at the bottom of this file.
// ============================================================================

// C++: #include copies another file (a "header") into this one before
//      compiling, which gives us access to the functions and classes declared
//      in it. <angle brackets> search the installed libraries and system
//      folders. "quotes" (used further down) search the sketch's own folder
//      first, then the libraries.
#include <SPI.h>            // SPI bus, used to talk to the thermocouple board
#include <avr/wdt.h>        // the AVR chip's watchdog timer (wdt_enable, wdt_reset)
#include <AltSoftSerial.h>  // extra serial port on D8/D9 for the Nextion screen

// ---------------- Thermocouple board ----------------

// C++: Lines starting with # are "preprocessor directives". The preprocessor
//      runs BEFORE the compiler and edits the source text. #define makes a
//      text substitution: everywhere USE_MAX31855 appears, it becomes 1.
#define USE_MAX31855 1  // 1 = MAX31855 board, 0 = MAX6675 board

// C++: #if / #else / #endif is "conditional compilation". Only one branch is
//      kept; the other is deleted before compiling, as if you never wrote it.
//      This lets one file support two different boards.
#if USE_MAX31855
#include "Adafruit_MAX31855.h"
#else
#include "max6675.h"
#endif

// C++: "const" means the value can never change after it is set. The compiler
//      will refuse to compile any line that tries to change it, and it can
//      often avoid using RAM for it at all (the Uno has only 2 KB of RAM).
//
// C++: uint8_t is an "unsigned 8-bit integer": a whole number from 0 to 255.
//      It uses 1 byte. Other integer types on the Uno:
//        int  / unsigned int   16 bits  (-32768..32767 / 0..65535)
//        long / unsigned long  32 bits  (about +/-2.1 billion / 0..4.29 billion)
//      Choosing the smallest type that fits saves RAM.
const uint8_t PIN_TC_CLK = 13;  // clock pin to the thermocouple board
const uint8_t PIN_TC_DO  = 12;  // data out from the board
const uint8_t PIN_TC_CS  = 10;  // chip select: tells the board "I'm talking to you"

// C++: This line creates an OBJECT named "thermocouple". Adafruit_MAX31855 is
//      a CLASS (a blueprint that bundles data and functions together). The
//      values in ( ) are passed to the class's CONSTRUCTOR, a special function
//      that runs once when the object is created to set it up.
//      Later we call its member functions with a dot: thermocouple.readFahrenheit()
#if USE_MAX31855
Adafruit_MAX31855 thermocouple(PIN_TC_CLK, PIN_TC_CS, PIN_TC_DO);
#else
MAX6675 thermocouple(PIN_TC_CLK, PIN_TC_CS, PIN_TC_DO);
#endif

// ---------------- Nextion screen ----------------
// AltSoftSerial is full duplex (unlike SoftwareSerial it can receive while
// sending), so screen commands are not lost. On the Uno it always uses
// D8 = RX (from Nextion TX) and D9 = TX (to Nextion RX).

// C++: An object created with no ( ) uses the class's default constructor.
AltSoftSerial nex;
const long NEX_BAUD = 9600;                 // Nextion factory default
// C++: Times are "unsigned long" because millis() (the Arduino clock, in
//      milliseconds since power-up) returns unsigned long. Mixing types in
//      comparisons can cause subtle bugs, so we match it.
const unsigned long NEX_UPDATE_MS = 1000;   // refresh the screen once a second
// Screen frames carry a check number: <S500,29500>  value + check must equal this
const long FRAME_CHECK_TOTAL = 30000;

// ---------------- Heaters / relays ----------------
const uint8_t NUM_HEATERS = 6;

// C++: This is an ARRAY: several values of the same type stored side by side.
//      [NUM_HEATERS] sets its size (6). The { } list fills it in order.
//      Arrays are numbered from 0, so RELAY_PINS[0] is 2 and RELAY_PINS[5] is 7.
//      Reading RELAY_PINS[6] would be a bug: it reads past the end, and C++
//      does NOT check this for you.
const uint8_t RELAY_PINS[NUM_HEATERS] = {2, 3, 4, 5, 6, 7};

// Most common relay modules turn ON when the input is LOW. If your heaters
// come on when they should be off, set this to false.
// C++: bool holds only true or false.
const bool RELAY_ACTIVE_LOW = true;

// Time-proportioning window for the one cycling relay. Mechanical relays
// wear out with switching, so keep this long (10 s). With solid-state relays
// it can be shortened to ~2000 ms for even tighter control.
const unsigned long WINDOW_MS    = 10000;
const unsigned long MIN_PULSE_MS = 1000;  // ignore on/off pulses shorter than this

// ---------------- Run settings (Fahrenheit) ----------------

// C++: These have no "const", so they are VARIABLES that can change while the
//      program runs (the screen and Serial Monitor change them).
//      They are declared outside any function, which makes them GLOBAL:
//      every function in the file can see them, and they keep their value
//      for as long as the board is powered.
//
// C++: float holds numbers with a decimal point (about 6-7 significant
//      digits). Writing 500.0 instead of 500 makes it clear it is a decimal.
//      On the Uno, "double" is the same size as float (32 bits).
float setpointF = 500.0;
unsigned int durationMin = 60;            // hold time once at temperature
const float MIN_SETPOINT_F = 100.0;
const unsigned int MAX_DURATION_MIN = 1440;  // 24 hours
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
//
// C++: The "UL" suffix makes each number an unsigned long. Without it, C++
//      would multiply them as 16-bit ints, and 10 * 60 * 1000 = 600000 is too
//      big for an int (max 32767): it would "overflow" and give a wrong
//      number. This is one of the most common Arduino bugs.
const unsigned long RUNAWAY_TIME_MS = 10UL * 60UL * 1000UL;  // 10 minutes
const float RUNAWAY_MIN_RISE_F = 15.0;

// ---------------- Timing ----------------
const unsigned long READ_INTERVAL_MS = 250;   // MAX6675 needs >= 220 ms between reads
const unsigned long PID_INTERVAL_MS  = 1000;  // PID runs on the 1 s average of readings

// ---------------- State ----------------
// Numbers are sent to the screen as aState, keep in sync with NEXTION.md
//
// C++: An ENUM ("enumeration") gives names to a set of related numbers, so the
//      code can say HEATING instead of a mystery 1. ": uint8_t" tells the
//      compiler to store it in 1 byte. RunState becomes a new TYPE, and the
//      variable "state" can hold one of these five values.
enum RunState : uint8_t { IDLE = 0, HEATING = 1, HOLDING = 2, DONE = 3, FAULT = 4 };
RunState state = IDLE;
unsigned long holdStartMs = 0;  // millis() time when the hold timer started

// C++: The * makes this a POINTER: instead of holding text itself, it holds
//      the memory ADDRESS of some text. __FlashStringHelper is the Arduino
//      type for text stored in flash memory with F("...") (explained in
//      stateName() below). nullptr means "points to nothing yet".
const __FlashStringHelper *faultReason = nullptr;

float outputPct = 0;  // PID output, 0..100 % of total heater power
float integral = 0;   // the PID's built-up "I" term

// C++: NAN means "Not A Number", a special float value meaning "no valid
//      reading yet". Any comparison with NAN is false, so it must be tested
//      with isnan(x), never with x == NAN.
float lastTempF = NAN;
float currentTempF = NAN;
float readingSum = 0;      // adds up the readings taken during one second...
uint8_t readingCount = 0;  // ...and counts them, to make an average
uint8_t badReads = 0;      // consecutive failed thermocouple readings

// Thermal runaway tracking
bool saturated = false;             // true while heaters are near full power
unsigned long saturatedSinceMs = 0;
float saturatedStartTempF = 0;

// Timestamps for the jobs loop() does at different rates
unsigned long lastReadMs = 0;
unsigned long lastPidMs = 0;
unsigned long lastNexMs = 0;
unsigned long windowStartMs = 0;
uint8_t windowFullHeaters = 0;     // heaters fully on in this window
unsigned long windowPartialMs = 0; // how long the one cycling heater is on

// C++: A char holds one character (1 byte). A char ARRAY is the classic C/C++
//      way to store text: the characters in a row, followed by a '\0' (zero)
//      byte that marks where the text ends. This is called a "C string".
//      16 bytes = up to 15 characters plus the '\0'.
char cmdBuf[16];     // USB command being typed, e.g. "S500"
uint8_t cmdLen = 0;  // how many characters are in it so far

char frameBuf[16];   // screen frame being received, e.g. "S500,29500"
uint8_t frameLen = 0;
bool inFrame = false;       // true between a '<' and a '>'
bool nexUpdateNow = false;  // set true to refresh the screen immediately
unsigned int badFrames = 0; // count of rejected screen frames, for debugging

// ---------------------------------------------------------------

// C++: A FUNCTION is a named, reusable block of code. The word before the name
//      is the RETURN TYPE: what the function gives back. This one returns a
//      bool. "return" sends the value back to whoever called it.
//
// C++: == tests "is equal" (a single = would ASSIGN instead, a classic bug).
//      || means OR: true if either side is true. && (used later) means AND.
bool isRunning() {
  return state == HEATING || state == HOLDING;
}

// C++: F("text") stores the text in FLASH memory (32 KB, where the program
//      lives) instead of copying it into RAM (only 2 KB). Every message in this
//      sketch uses F() so the Uno doesn't run out of RAM. This function returns
//      a pointer to one of those flash strings.
//
// C++: switch picks a "case" that matches the value. Each case here returns
//      immediately, so no "break" is needed (see runCommand for break).
const __FlashStringHelper *stateName() {
  switch (state) {
    case IDLE:    return F("IDLE");
    case HEATING: return F("HEATING");
    case HOLDING: return F("HOLDING");
    case DONE:    return F("DONE");
    case FAULT:   return F("FAULT");
  }
  return F("?");  // only reached if state somehow had an unexpected value
}

// Turn relay number i (0..5) on or off.
//
// C++: "void" means the function returns nothing. (uint8_t i, bool on) are
//      PARAMETERS: values handed in by the caller. They are copies, so
//      changing them inside the function wouldn't affect the caller.
//
// C++: condition ? A : B is the TERNARY operator: "if condition then A else B"
//      in one expression.
//      on != RELAY_ACTIVE_LOW is a neat trick: with an active-LOW relay,
//      on=true gives false (LOW = relay on) and on=false gives true (HIGH =
//      relay off). With an active-HIGH relay it is the other way round.
void writeRelay(uint8_t i, bool on) {
  digitalWrite(RELAY_PINS[i], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// C++: A FOR loop repeats code. for (start; keep-going-while; after-each-pass)
//      Here i starts at 0, the loop runs while i < 6, and i++ adds 1 after
//      each pass, so the body runs for i = 0,1,2,3,4,5. "i" only exists
//      inside the loop (its SCOPE is the loop).
void allHeatersOff() {
  for (uint8_t i = 0; i < NUM_HEATERS; i++) writeRelay(i, false);
}

// Something unsafe happened: turn everything off and stay off until reset.
void tripFault(const __FlashStringHelper *reason) {
  // Only report the first fault; later calls just keep the heaters off.
  if (state != FAULT) {
    state = FAULT;
    faultReason = reason;  // remember the message to print later
    Serial.print(F("FAULT: "));
    Serial.print(reason);
    // C++: print() sends text; println() sends text plus a new line.
    Serial.println(F(" - all heaters latched OFF, reset the board to clear"));
    nexUpdateNow = true;
  }
  outputPct = 0;
  allHeatersOff();
}

// Spread outputPct across the heaters: N fully on, one relay time-proportioned.
// The split is only recalculated at the start of each window so relays switch
// at most twice per window.
//
// Example: outputPct = 40%  ->  40% of 6 heaters = 2.4 heaters
//   heaters 0 and 1 fully on, heater 2 on for 0.4 x 10 s = 4 s of each window
void applyHeaters(unsigned long now) {
  // C++: ! means NOT. An early "return" exits the function right away.
  if (!isRunning()) {
    allHeatersOff();
    return;
  }

  // C++: "now - windowStartMs" is how long since the window started. Doing
  //      it as a subtraction of unsigned longs keeps working even when
  //      millis() rolls over back to 0 after about 49.7 days, because unsigned
  //      math wraps around. Always compare times this way, never with
  //      "now >= windowStartMs + WINDOW_MS".
  if (now - windowStartMs >= WINDOW_MS) {
    windowStartMs = now;
    float elements = outputPct / 100.0 * NUM_HEATERS;  // 0.0 .. 6.0

    // C++: (uint8_t)elements is a CAST: it converts the float to a whole
    //      number by chopping off the decimals (2.4 becomes 2, it does not
    //      round). That gives the number of fully-on heaters.
    windowFullHeaters = (uint8_t)elements;
    // The leftover fraction (2.4 - 2 = 0.4) becomes on-time for one heater.
    windowPartialMs = (unsigned long)((elements - windowFullHeaters) * WINDOW_MS);

    // Avoid very short relay clicks, which wear relays for no real benefit.
    if (windowPartialMs < MIN_PULSE_MS) {
      windowPartialMs = 0;
    } else if (windowPartialMs > WINDOW_MS - MIN_PULSE_MS) {
      windowPartialMs = 0;
      windowFullHeaters++;  // C++: ++ adds 1
    }
  }

  unsigned long intoWindow = now - windowStartMs;
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    // Heater i is on if it is one of the fully-on ones, OR it is the cycling
    // one and we are still inside its on-time for this window.
    bool on = (i < windowFullHeaters) ||
              (i == windowFullHeaters && intoWindow < windowPartialMs);
    writeRelay(i, on);
  }
}

// The PID calculation. Works out how much heater power (0..100 %) is needed.
//   tempF = latest averaged temperature, dtS = seconds since the last run
void runPid(float tempF, float dtS) {
  // C++: A variable declared inside a function is LOCAL: it is created each
  //      time the function runs and disappears when it ends.
  float error = setpointF - tempF;  // positive = too cold, negative = too hot

  // I term: slowly builds up while there is an error. Only allowed near the
  // setpoint, so it doesn't build up a huge amount during the long warm-up.
  // C++: fabs() is the absolute value of a float (drops the minus sign).
  if (fabs(error) < INTEGRAL_BAND_F) {
    // C++: a += b is shorthand for a = a + b.
    integral += Ki * error * dtS;
    // constrain(x, low, high) is an Arduino helper that clamps x into a range.
    integral = constrain(integral, 0.0, 100.0);
  }

  // D term: how fast the temperature is changing (F per second).
  // Derivative on measurement avoids a kick when the setpoint changes
  float dInput = isnan(lastTempF) ? 0.0 : (tempF - lastTempF) / dtS;
  lastTempF = tempF;

  // P + I + D, clamped to 0..100 %
  outputPct = constrain(Kp * error + integral - Kd * dInput, 0.0, 100.0);
}

// Detect heaters running flat out without the temperature rising, which
// usually means the thermocouple has fallen out of the enclosure.
void checkRunaway(unsigned long now, float tempF) {
  if (outputPct < 95.0) {
    saturated = false;  // not at full power, nothing to check
    return;
  }
  if (!saturated) {
    // Just reached full power: start a 10-minute check from here.
    saturated = true;
    saturatedSinceMs = now;
    saturatedStartTempF = tempF;
  } else if (now - saturatedSinceMs >= RUNAWAY_TIME_MS) {
    // 10 minutes at full power: did the temperature rise enough?
    if (tempF - saturatedStartTempF < RUNAWAY_MIN_RISE_F) {
      tripFault(F("thermal runaway (full power but temperature not rising)"));
    }
    // Start the next 10-minute check.
    saturatedSinceMs = now;
    saturatedStartTempF = tempF;
  }
}

// Take one thermocouple reading and add it to this second's average.
void readSensor() {
  // C++: Calling a MEMBER FUNCTION of the thermocouple object. The library
  //      returns NAN if the thermocouple is disconnected or shorted.
  float tempF = thermocouple.readFahrenheit();

  if (isnan(tempF)) {
    Serial.print(F("Thermocouple fault"));
#if USE_MAX31855
    uint8_t fault = thermocouple.readError();
    // C++: & here is BITWISE AND. readError() packs several yes/no flags into
    //      the bits of one number; "fault & FLAG" is non-zero only if that
    //      flag's bit is set. (Don't confuse with &&, which is logical AND.)
    if (fault & MAX31855_FAULT_OPEN)      Serial.print(F(" OPEN"));
    if (fault & MAX31855_FAULT_SHORT_GND) Serial.print(F(" SHORT_GND"));
    if (fault & MAX31855_FAULT_SHORT_VCC) Serial.print(F(" SHORT_VCC"));
#else
    Serial.print(F(" OPEN"));  // the MAX6675 can only detect an open circuit
#endif
    Serial.println();
    // C++: ++badReads (++ BEFORE the name) adds 1 first, then the new value is
    //      compared. badReads++ (++ AFTER) would compare the old value first.
    if (++badReads >= MAX_BAD_READS) tripFault(F("thermocouple not reading"));
    return;
  }

  badReads = 0;  // a good reading resets the fault counter
  readingSum += tempF;
  readingCount++;
}

// ---------------- Run control (shared by USB and the screen) ----------------

// Returns true if the new setpoint was accepted, false if out of range.
// Returning a bool like this lets the caller know whether it worked.
bool setSetpoint(float v) {
  if (v < MIN_SETPOINT_F || v > MAX_SAFE_F - 25) return false;
  setpointF = v;
  return true;
}

bool setDuration(long minutes) {
  // C++: (long)MAX_DURATION_MIN converts the unsigned int to long so both
  //      sides of the comparison are the same type. Comparing signed and
  //      unsigned numbers can give surprising results with negative values.
  if (minutes < 1 || minutes > (long)MAX_DURATION_MIN) return false;
  durationMin = minutes;
  return true;
}

// Repeated starts while already running are ignored, so the screen can
// safely resend Start without restarting the timer. A fault blocks starting.
void startRun() {
  if (state != IDLE && state != DONE) return;
  // Reset the PID so leftovers from a previous run don't affect this one.
  integral = 0;
  lastTempF = NAN;
  saturated = false;
  state = HEATING;
  // Calculate a power level right away so heating starts immediately
  // instead of waiting up to a second for the next PID update.
  // C++: 1000.0 (with a decimal point) forces decimal division. 1000 / 1000
  //      would be fine, but e.g. 500 / 1000 with whole numbers gives 0, not 0.5.
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

// Move between HEATING -> HOLDING -> DONE. Called once a second during a run.
void updateRunState(unsigned long now) {
  if (state == HEATING && currentTempF >= setpointF - READY_BAND_F) {
    state = HOLDING;
    holdStartMs = now;  // the hold timer starts now
    Serial.println(F("At temperature - hold timer started"));
  }
  // C++: durationMin * 60000UL: the UL makes the multiply happen in 32 bits.
  //      60 minutes x 60000 = 3,600,000, far too big for a 16-bit int.
  if (state == HOLDING && now - holdStartMs >= durationMin * 60000UL) {
    state = DONE;
    outputPct = 0;
    allHeatersOff();
    Serial.println(F("Run complete - heaters off"));
  }
}

// Seconds remaining in the hold, for the screen.
unsigned long secondsLeft(unsigned long now) {
  unsigned long total = durationMin * 60UL;
  if (state == HOLDING) {
    // C++: Dividing two whole numbers gives a whole number (the remainder is
    //      dropped), so this is whole seconds elapsed.
    unsigned long elapsed = (now - holdStartMs) / 1000;
    // Never return a negative-looking value: unsigned numbers can't go below
    // 0, they would wrap around to a huge number instead.
    return elapsed >= total ? 0 : total - elapsed;
  }
  if (state == DONE) return 0;
  return total;  // not started holding yet: show the full time
}

// ---------------- USB Serial commands ----------------

void printSettings() {
  // C++: print(value, 1) prints a float with 1 decimal place. print() has
  //      several versions that accept different types (text, int, float...).
  //      Having several functions with the same name but different parameters
  //      is called OVERLOADING; the compiler picks the right one.
  Serial.print(F("Setpoint="));  Serial.print(setpointF, 1);
  Serial.print(F("F Hold="));    Serial.print(durationMin);
  Serial.print(F("min Kp="));    Serial.print(Kp, 3);
  Serial.print(F(" Ki="));       Serial.print(Ki, 4);
  Serial.print(F(" Kd="));       Serial.print(Kd, 3);
  Serial.print(F(" State="));    Serial.print(stateName());
  Serial.print(F(" BadFrames=")); Serial.println(badFrames);
}

// Run one command typed in the Serial Monitor, e.g. "S500" or "G".
//
// C++: "char *cmd" is a pointer to the first character of a C string.
//      cmd[0] is the first character, cmd[1] the second, and so on.
void runCommand(char *cmd) {
  // C++: toupper() turns 's' into 'S', so commands work in either case.
  //      'S' in single quotes is ONE character; "S" in double quotes is a
  //      string (the character plus a '\0' terminator). They are different types.
  char key = toupper(cmd[0]);
  switch (key) {
    case '?': printSettings(); return;
    case 'G': startRun(); nexUpdateNow = true; return;
    case 'X': stopRun();  nexUpdateNow = true; return;
  }

  // The remaining commands need a number after the letter.
  if (cmd[1] == '\0') key = 0;  // no number given -> falls to "default" below
  // C++: cmd + 1 is POINTER ARITHMETIC: a pointer to the 2nd character, so
  //      atof() (text to float) reads "500" from "S500".
  float v = atof(cmd + 1);

  // C++: Each case ends with "break", which jumps out of the switch. Without
  //      it, execution would "fall through" into the next case too.
  //      "default" runs when no case matches.
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
  nexUpdateNow = true;  // show the change on the screen right away
  printSettings();
}

// Collect characters typed in the Serial Monitor until Enter is pressed.
// This never waits: it handles whatever has arrived, then returns, so the
// rest of loop() keeps running (heater control must never be held up).
void handleSerial() {
  // C++: A WHILE loop repeats as long as its condition is true.
  //      Serial.available() is how many received characters are waiting.
  while (Serial.available()) {
    char c = Serial.read();
    // '\n' is the newline character and '\r' carriage return: what the
    // Serial Monitor sends when you press Enter.
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';  // terminate the C string
        runCommand(cmdBuf);     // C++: an array name passes as a pointer to its first element
        cmdLen = 0;             // ready for the next command
      }
    // C++: sizeof(cmdBuf) is the array's size in bytes (16). Keeping 1 byte
    //      spare for the '\0' prevents writing past the end of the array,
    //      a "buffer overflow", which can corrupt other variables.
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      // C++: cmdLen++ uses the current value as the index, THEN adds 1.
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

// Check and act on one complete frame (the text between < and >).
void handleFrame(char *f) {
  bool ok = false;

  // C++: strcmp() compares two C strings and returns 0 when they are equal.
  //      (You can't compare C strings with ==; that would compare their
  //      memory addresses, not their text.)
  if (strcmp(f, "GO") == 0) {
    startRun();
    ok = true;
  } else if (strcmp(f, "X") == 0) {
    stopRun();
    ok = true;
  } else if (f[0] == 'S' || f[0] == 'M') {
    // C++: strtol() ("string to long") reads a whole number from text. Its
    //      second argument, &end, is the ADDRESS of our pointer "end", so
    //      strtol can set it to point at the first character it did NOT
    //      read. That tells us exactly where the number stopped.
    //      Example: f = "S500,29500" -> value = 500, end points at ','
    char *end;
    long value = strtol(f + 1, &end, 10);  // 10 = base 10 (normal decimal)
    // C++: *end DEREFERENCES the pointer: it gives the character it points at.
    //      end != f + 1 checks that at least one digit was actually read.
    if (end != f + 1 && *end == ',') {
      char *checkStart = end + 1;  // the character after the comma
      long check = strtol(checkStart, &end, 10);
      // Accept only if: digits were read, nothing extra follows them
      // (*end == '\0' is the end of the string), and the check adds up.
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

// Read bytes from the screen and assemble them into frames.
// This is a small "state machine": inFrame remembers whether we are currently
// between a '<' and a '>', and each byte is handled based on that.
void handleNextion() {
  while (nex.available()) {
    char c = nex.read();
    if (c == '<') {
      // Start of a new frame (also restarts if a previous frame was cut off).
      inFrame = true;
      frameLen = 0;
    } else if (!inFrame) {
      // ignore bytes between frames
    } else if (c == '>') {
      // End of the frame: terminate the string and process it.
      inFrame = false;
      frameBuf[frameLen] = '\0';
      handleFrame(frameBuf);
    // C++: Characters are stored as numbers (ASCII codes), so they can be
    //      compared with < and >. ' ' (32) to '~' (126) are the normal
    //      printable characters; anything else means the data is garbled.
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
//
// Example of what gets sent:  tTemp.txt="499.6"  followed by FF FF FF

// C++: 0xFF is a number written in HEXADECIMAL (base 16): 0xFF = 255.
//      write() sends a raw byte, while print() sends readable text
//      (print(255) would send the three characters '2','5','5').
void nexEnd() {
  nex.write(0xFF); nex.write(0xFF); nex.write(0xFF);
}

// Set a number on the screen, e.g. nexVal(F("aSet"), 500) sends  aSet.val=500
void nexVal(const __FlashStringHelper *obj, long v) {
  nex.print(obj); nex.print(F(".val=")); nex.print(v);
  nexEnd();
}

// Start setting the text of a screen item: sends  name.txt="
// C++: \" inside a string is an ESCAPE SEQUENCE for a literal " character
//      (a plain " would end the string).
void nexTxtBegin(const __FlashStringHelper *obj) {
  nex.print(obj); nex.print(F(".txt=\""));
}

// Finish setting text: closing quote, then the FF FF FF terminator.
void nexTxtEnd() {
  nex.print('"');  // C++: '"' is a single double-quote character
  nexEnd();
}

// Print a number with at least 2 digits (7 -> "07"), for the time display.
void print2Digits(unsigned long v) {
  if (v < 10) nex.print('0');
  nex.print(v);
}

// Send everything the screen displays. Called once a second, and right away
// after any change.
void updateNextion(unsigned long now) {
  nexTxtBegin(F("tTemp"));
  // C++: if/else without { } braces controls only the single next statement.
  if (isnan(currentTempF)) nex.print(F("---"));
  else nex.print(currentTempF, 1);
  nexTxtEnd();

  nexTxtBegin(F("tStatus"));
  nex.print(stateName());
  nexTxtEnd();

  // Format the seconds left as H:MM:SS
  // C++: % is the MODULO (remainder) operator: 125 % 60 = 5.
  //      So (s / 60) % 60 is minutes past the hour, s % 60 is seconds.
  unsigned long s = secondsLeft(now);
  nexTxtBegin(F("tLeft"));
  nex.print(s / 3600); nex.print(':');
  print2Digits((s / 60) % 60); nex.print(':');
  print2Digits(s % 60);
  nexTxtEnd();

  nexTxtBegin(F("tOut"));
  // C++: A cast to int just drops the decimals (41.8 -> 41). Adding 0.5
  //      first ROUNDS to the nearest whole number instead (41.8 + 0.5 = 42.3 -> 42).
  nex.print((int)(outputPct + 0.5)); nex.print('%');
  nexTxtEnd();

  // Values the screen compares against to confirm its commands arrived
  nexVal(F("aSet"), (long)(setpointF + 0.5));
  nexVal(F("aMin"), durationMin);
  // C++: An enum value converts automatically to its number (HOLDING -> 2).
  nexVal(F("aState"), state);
  nexVal(F("link"), 0);  // screen shows NO LINK if this isn't reset
}

// ---------------------------------------------------------------

// One line of status for the Serial Monitor / Serial Plotter, once a second.
void printStatus() {
  // label:value format works in both the Serial Monitor and Serial Plotter
  Serial.print(F("Temp_F:"));      Serial.print(currentTempF, 1);
  Serial.print(F(" Setpoint_F:")); Serial.print(setpointF, 1);
  Serial.print(F(" Output_pct:")); Serial.print(outputPct, 1);
  Serial.print(F(" State:"));      Serial.println(state);
}

// C++: setup() runs ONCE when the board powers up or is reset.
void setup() {
  // Set relays to OFF before enabling them as outputs so nothing blips on at boot
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    writeRelay(i, false);
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelay(i, false);
  }

  Serial.begin(9600);   // USB serial to the computer
  nex.begin(NEX_BAUD);  // serial to the Nextion screen
  delay(500);  // let the thermocouple board and screen start up
#if USE_MAX31855
  if (!thermocouple.begin()) Serial.println(F("MAX31855 init failed"));
#endif
  nex.print(F("bkcmd=0"));  // stop the screen replying to every instruction
  nexEnd();

  Serial.println(F("Heater controller started - press Start (or send G) to run"));
  printSettings();

  // The watchdog is a hardware timer that resets the board if it isn't
  // "fed" (wdt_reset) within 2 seconds. If the code ever gets stuck, the
  // board restarts, and setup() turns all heaters off again.
  wdt_enable(WDTO_2S);
}

// C++: loop() runs over and over, thousands of times per second.
//
// Notice there is no delay() in here. Instead, each job checks "has enough
// time passed since I last ran?" using millis(). This lets several jobs run at
// different rates without blocking each other, a common Arduino pattern
// sometimes called "blink without delay".
void loop() {
  wdt_reset();  // feed the watchdog: "the program is still running fine"
  unsigned long now = millis();  // read the clock once and reuse it

  // Every pass: check for typed commands and screen commands.
  handleSerial();
  handleNextion();

  // Every 250 ms: take a thermocouple reading.
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;
    readSensor();
  }

  // Every 1 s: average the readings, check safety, run the PID.
  if (now - lastPidMs >= PID_INTERVAL_MS) {
    float dtS = (now - lastPidMs) / 1000.0;  // actual seconds since last time
    lastPidMs = now;

    if (readingCount > 0) {
      currentTempF = readingSum / readingCount;  // average of this second's readings
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

  // Every 1 s, or right away after a change: refresh the screen.
  if (nexUpdateNow || now - lastNexMs >= NEX_UPDATE_MS) {
    nexUpdateNow = false;
    lastNexMs = now;
    updateNextion(now);
  }

  // Every pass: switch the relays to match the current power level.
  applyHeaters(now);
}
