// Heater enclosure controller - ESP32 version
// (Same heater control and safety as heater_controller.ino for the Uno. The
//  differences are the pins, the watchdog, and the screen: this version uses
//  a "no-code" Nextion screen, see NEXTION_ESP32.md.)
//
// Holds the enclosure at a set temperature for a set time using:
//   - one Type K thermocouple read through a MAX31855 or MAX6675 board
//   - PID control
//   - 6 heating elements switched by relays, staged so that the PID output
//     is spread across the elements (e.g. 50% power = 3 elements fully on)
//     and only ONE relay ever cycles to make up the fractional part.
//   - a Nextion NX4827P043 (4.3", 480x272) touch screen to set the
//     temperature and time, start and stop. The screen holds no code at all:
//     it only reports which button was tapped, and the ESP32 does the rest.
//
// Run sequence: IDLE -> (Start) -> HEATING -> reaches temp -> HOLDING (timer
// counts down) -> time up -> DONE. Heaters are only on in HEATING/HOLDING.
//
// Board: Arduino IDE > Boards Manager > install "esp32" by Espressif Systems,
//        then select "ESP32 Dev Module" (or your exact ESP32 board).
//
// Libraries (Arduino IDE > Library Manager):
//   MAX31855 board: "Adafruit MAX31855 library"
//   MAX6675 board:  "MAX6675 library" by Adafruit
//   (No AltSoftSerial needed: the ESP32 has spare hardware serial ports.)
//
// !! The ESP32 runs at 3.3 V. Its pins must never receive 5 V. !!
//
// Wiring (GPIO numbers, as printed on most ESP32 dev boards):
//   Thermocouple board: VCC -> 3V3 (NOT 5V, so its data output is 3.3 V),
//                       GND -> GND, SCK/CLK -> GPIO18, SO/DO -> GPIO19, CS -> GPIO23
//   Relays: heater 1..6 relay inputs -> GPIO 25, 26, 27, 32, 33, 4
//           (see the relay notes below: the relays must switch reliably from 3.3 V)
//   Nextion: TX (blue) -> GPIO16, RX (yellow) -> GPIO17, GND -> GND,
//            red 5V wire from its own 5 V supply (Nextion serial is 3.3 V level)
//   E-stop: normally-closed (NC) contact between GPIO13 and GND.
//           No E-stop fitted yet? Put a wire jumper from GPIO13 to GND, or the
//           controller will stay in FAULT.
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
#include <esp_task_wdt.h>   // the ESP32's task watchdog timer

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
//      will refuse to compile any line that tries to change it.
//
// C++: uint8_t is an "unsigned 8-bit integer": a whole number from 0 to 255.
//      It uses 1 byte. Integer sizes depend on the chip. On the ESP32:
//        int  / unsigned int   32 bits  (about +/-2.1 billion / 0..4.29 billion)
//        long / unsigned long  32 bits  (same as int on the ESP32)
//      On the Uno, int is only 16 bits. Code that must work on both boards
//      should not rely on int being big (see the UL note further down).
//
// ESP32 pin choice: GPIO 0, 2, 5, 12 and 15 are "strapping" pins that are read
// at boot and may glitch, GPIO 6-11 are wired to the flash chip, and GPIO 34-39
// are input-only with no pull-ups. None of those are used below.
const uint8_t PIN_TC_CLK = 18;  // clock pin to the thermocouple board
const uint8_t PIN_TC_DO  = 19;  // data out from the board
const uint8_t PIN_TC_CS  = 23;  // chip select: tells the board "I'm talking to you"

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
// The ESP32 has three hardware serial ports. Serial (UART0) is USB, and
// Serial2 (UART2) is free for the screen. Hardware serial sends and receives
// at the same time in the background, so screen commands are not lost.
//
// C++: The & makes "nex" a REFERENCE: another name for the existing Serial2
//      object, not a copy. Everything below can say nex.print(...), and to
//      move the screen to a different port you change only this one line.
HardwareSerial &nex = Serial2;
const uint8_t PIN_NEX_RX = 16;  // ESP32 receives here  <- Nextion TX (blue)
const uint8_t PIN_NEX_TX = 17;  // ESP32 sends here     -> Nextion RX (yellow)
const long NEX_BAUD = 9600;                 // Nextion factory default
// C++: Times are "unsigned long" because millis() (the Arduino clock, in
//      milliseconds since power-up) returns unsigned long. Mixing types in
//      comparisons can cause subtle bugs, so we match it.
const unsigned long NEX_UPDATE_MS = 1000;   // refresh the screen once a second

// Button component IDs. Nextion Editor numbers components in the order they
// are added to the page (the page itself is 0). NEXTION_ESP32.md says to add
// the six buttons FIRST, in this order, so they get IDs 1-6. If yours differ,
// check each button's "id" attribute in Nextion Editor and change these.
const uint8_t ID_TEMP_DN = 1;  // bTempDn
const uint8_t ID_TEMP_UP = 2;  // bTempUp
const uint8_t ID_MIN_DN  = 3;  // bMinDn
const uint8_t ID_MIN_UP  = 4;  // bMinUp
const uint8_t ID_START   = 5;  // bStart
const uint8_t ID_STOP    = 6;  // bStop

// How much each tap of + / - changes the settings
const float TEMP_STEP_F = 5.0;
const unsigned int MIN_STEP_SHORT = 5;       // minutes per tap up to 2 hours...
const unsigned int MIN_STEP_LONG  = 15;      // ...and above 2 hours
const unsigned int MIN_STEP_SWITCH = 120;

// Bytes the Nextion sends back, each followed by FF FF FF:
//   65 page id event  - a button was touched (event 00 = released, 01 = pressed)
//   66 page           - reply to our "sendme" ping: "I'm here, on this page"
//   88                - the screen has just started up
const uint8_t NEX_TOUCH = 0x65;
const uint8_t NEX_PAGE  = 0x66;
const uint8_t NEX_READY = 0x88;

// ---------------- Heaters / relays ----------------
const uint8_t NUM_HEATERS = 6;

// C++: This is an ARRAY: several values of the same type stored side by side.
//      [NUM_HEATERS] sets its size (6). The { } list fills it in order.
//      Arrays are numbered from 0, so RELAY_PINS[0] is 25 and RELAY_PINS[5] is 4.
//      Reading RELAY_PINS[6] would be a bug: it reads past the end, and C++
//      does NOT check this for you.
//
// These pins stay quiet during boot, so no heater flicks on at power-up.
const uint8_t RELAY_PINS[NUM_HEATERS] = {25, 26, 27, 32, 33, 4};

// Most common relay modules turn ON when the input is LOW. If your heaters
// come on when they should be off, set this to false.
//
// 3.3 V RELAY WARNING: an ESP32 pin only goes up to 3.3 V. A 5 V active-LOW
// relay module may not fully turn OFF from a 3.3 V "high", so a heater could
// stay on. Use one of these:
//   - Solid-state relays with a 3-32 V DC input (best: work directly at 3.3 V,
//     and set RELAY_ACTIVE_LOW = false).
//   - An optocoupler relay module with a JD-VCC jumper: remove the jumper,
//     power VCC from 3V3 and JD-VCC from 5V.
//   - A relay module labelled "3.3 V compatible" or "high/low level trigger".
// Test with the relay_test sketch before connecting heaters (it uses these
// same pins automatically when an ESP32 board is selected).
//
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
//      On the ESP32, "double" is twice as precise (64 bits) but slower;
//      float is plenty for temperatures.
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
// C++: The "UL" suffix makes each number an unsigned long. On the ESP32 int
//      is 32 bits, so 10 * 60 * 1000 = 600000 would fit anyway. But on a
//      16-bit-int board like the Uno it would "overflow" (max 32767) and give
//      a wrong number, so writing UL is a good habit that keeps code portable.
const unsigned long RUNAWAY_TIME_MS = 10UL * 60UL * 1000UL;  // 10 minutes
const float RUNAWAY_MIN_RISE_F = 15.0;

// Overshoot: during a run, shut off if the temperature gets this far ABOVE the
// setpoint (e.g. setpoint 500 F -> fault at 520 F). MAX_SAFE_F above is still
// the absolute limit whatever the setpoint is.
const float OVERSHOOT_LIMIT_F = 20.0;

// E-stop button on GPIO13, wired as a normally-closed (NC) contact to GND.
// Normal: contact closed, pin reads LOW. Pressed OR wire broken: contact open,
// the pull-up makes the pin read HIGH -> heaters latched off.
// Using NC means a cut or loose wire also stops the heaters ("fail-safe").
//
// The ESP32's internal pull-up is weak (~45 kOhm). If the E-stop wire is long
// or runs near mains wiring, add a 10 kOhm resistor from GPIO13 to 3V3 so
// electrical noise can't cause false trips. Never connect it to 5V.
const uint8_t PIN_ESTOP = 13;
const unsigned long ESTOP_DEBOUNCE_MS = 20;  // must read "open" this long (filters electrical noise)

// Screen link: every second the ESP32 sends the screen "sendme", and the
// screen automatically replies with its page number. During a run, if no
// valid reply or touch arrives for this long, the heaters are latched off. Set REQUIRE_SCREEN_LINK to false only for bench testing over
// USB without a screen connected.
const bool REQUIRE_SCREEN_LINK = true;
const unsigned long SCREEN_TIMEOUT_MS = 5000;

// ---------------- Timing ----------------
const unsigned long READ_INTERVAL_MS = 250;   // MAX6675 needs >= 220 ms between reads
const unsigned long PID_INTERVAL_MS  = 1000;  // PID runs on the 1 s average of readings

// ---------------- State ----------------
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

// Overshoot check: only "armed" once the temperature is at or below the limit,
// so lowering the setpoint mid-run (or starting while already hot) gives the
// enclosure time to cool down instead of faulting straight away.
bool overshootArmed = false;

// E-stop tracking
bool estopOpen = false;            // true while the E-stop pin reads HIGH (open)
unsigned long estopOpenSinceMs = 0;

// Screen link tracking
bool screenSeen = false;           // has any valid frame arrived since power-up?
unsigned long lastScreenMs = 0;    // millis() of the last valid frame

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

// C++: uint8_t arrays are handy for raw bytes (numbers 0-255) rather than text.
uint8_t nexBuf[8];          // bytes of the message being received from the screen
uint8_t nexLen = 0;
uint8_t nexFFCount = 0;     // how many FF bytes in a row we've just seen
bool nexBad = false;        // true if the current message got garbled
bool nexUpdateNow = false;  // set true to refresh the screen immediately
bool beatOn = false;        // the blinking "alive" dot on the screen
unsigned int badFrames = 0; // count of unrecognised screen messages, for debugging

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

// C++: F("text") marks text to be kept in flash memory instead of RAM. On the
//      Uno (2 KB of RAM) that matters a lot. The ESP32 has plenty of RAM and
//      keeps constant text in flash anyway, so F() does nothing extra here, but
//      it is kept so this file matches the Uno version. This function returns
//      a pointer to one of those strings.
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

// Shut off if the temperature climbs more than OVERSHOOT_LIMIT_F above the
// setpoint during a run. Called once a second while running.
void checkOvershoot(float tempF) {
  float limit = setpointF + OVERSHOOT_LIMIT_F;
  if (!overshootArmed) {
    // Not armed yet: wait until we are at or below the limit, then start checking.
    if (tempF <= limit) overshootArmed = true;
    return;
  }
  if (tempF > limit) {
    tripFault(F("temperature more than 20 F over setpoint"));
  }
}

// Check the E-stop button. Called on every pass of loop(), so it reacts
// within ESTOP_DEBOUNCE_MS (20 ms) of the button being pressed.
void checkEstop(unsigned long now) {
  // C++: digitalRead() returns HIGH or LOW. With INPUT_PULLUP (see setup),
  //      an open contact reads HIGH and a contact closed to GND reads LOW.
  if (digitalRead(PIN_ESTOP) == LOW) {
    estopOpen = false;  // contact closed: all normal
    return;
  }
  if (!estopOpen) {
    // Just went open: note the time, and confirm it stays open (not noise).
    estopOpen = true;
    estopOpenSinceMs = now;
  } else if (now - estopOpenSinceMs >= ESTOP_DEBOUNCE_MS) {
    tripFault(F("E-stop pressed or E-stop wire broken"));
  }
}

// C++: This function is small but gets its own name because it is used in
//      two places (startRun and checkScreenLink). Naming a condition makes
//      the code read like a sentence: if (!screenLinkOk(now)) ...
bool screenLinkOk(unsigned long now) {
  return screenSeen && now - lastScreenMs < SCREEN_TIMEOUT_MS;
}

// During a run, shut off if the screen has gone quiet.
//
// C++: This reads millis() fresh instead of using loop()'s "now". A frame
//      handled earlier in this same pass may have stored a lastScreenMs a
//      millisecond AFTER "now", and with unsigned numbers now - lastScreenMs
//      would then wrap around to about 4.29 billion instead of -1, which
//      looks like a lost link. Rule: only subtract an older time from a newer one.
void checkScreenLink() {
  if (REQUIRE_SCREEN_LINK && isRunning() && !screenLinkOk(millis())) {
    tripFault(F("screen link lost"));
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
  // Lowering the setpoint mid-run could leave the enclosure more than 20 F
  // above the new target. Disarm the overshoot check until it cools down.
  if (v < setpointF) overshootArmed = false;
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
  // Don't start a run that would immediately fault for having no screen link.
  if (REQUIRE_SCREEN_LINK && !screenLinkOk(millis())) {
    Serial.println(F("Can't start: no link to the screen"));
    return;
  }
  // Reset the PID so leftovers from a previous run don't affect this one.
  integral = 0;
  lastTempF = NAN;
  saturated = false;
  overshootArmed = false;  // arms itself once the temperature is below the limit
  state = HEATING;
  // Calculate a power level right away so heating starts immediately
  // instead of waiting up to a second for the next PID update.
  // C++: 1000.0 (with a decimal point) forces decimal division. 1000 / 1000
  //      would be fine, but e.g. 500 / 1000 with whole numbers gives 0, not 0.5.
  if (!isnan(currentTempF)) runPid(currentTempF, PID_INTERVAL_MS / 1000.0);
  windowStartMs = millis() - WINDOW_MS;  // start a fresh relay window now
  // Clear the previous run's relay split so no relay follows stale values
  // before the fresh window is calculated.
  windowFullHeaters = 0;
  windowPartialMs = 0;
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

// Send one complete instruction, e.g. nexSend(F("sendme"))
void nexSend(const __FlashStringHelper *cmd) {
  nex.print(cmd);
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
// after any change. The ESP32 is the only source of what the screen shows,
// so the screen can never show a setting the ESP32 isn't actually using.
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

  // Why it faulted (blank otherwise), so the operator knows what happened
  nexTxtBegin(F("tMsg"));
  if (state == FAULT) nex.print(faultReason);
  nexTxtEnd();

  nexTxtBegin(F("tOut"));
  // C++: A cast to int just drops the decimals (41.8 -> 41). Adding 0.5
  //      first ROUNDS to the nearest whole number instead (41.8 + 0.5 = 42.3 -> 42).
  nex.print((int)(outputPct + 0.5)); nex.print('%');
  nexTxtEnd();

  // The settings, shown between the - and + buttons
  nexTxtBegin(F("tSet"));
  nex.print((long)(setpointF + 0.5));
  nexTxtEnd();

  nexTxtBegin(F("tMin"));
  nex.print(durationMin);
  nexTxtEnd();

  // Blinking dot: it changes on every update, so if the ESP32 ever stops
  // talking to the screen, the dot freezes and you can see the link is down.
  // C++: ! flips a bool: true becomes false, false becomes true.
  beatOn = !beatOn;
  nexTxtBegin(F("tBeat"));
  if (beatOn) nex.print('*');
  nexTxtEnd();

  // Ping. The screen answers 66 <page> FF FF FF all by itself, which tells
  // us the link is working (see handleNexMessage).
  nexSend(F("sendme"));
}

// ---------------- Nextion: receiving ----------------
// The screen has no code of its own. It only sends these built-in messages,
// each ending with three FF bytes:
//   65 00 05 00 FF FF FF  - on page 0, the button with id 5 (Start) was released
//   66 00 FF FF FF        - reply to our "sendme" ping: "I'm on page 0"
//   88 FF FF FF           - the screen has just powered up
// A message is only accepted if it has exactly the right length and type, so
// a garbled message is dropped instead of being misread as a button tap.

// Record that a valid message just arrived, so the screen link counts as alive.
void markScreenAlive() {
  screenSeen = true;
  lastScreenMs = millis();
}

// Change the hold time by one tap: 5-minute steps up to 2 hours, 15 above.
void stepDuration(bool up) {
  long m = durationMin;
  if (up) {
    m += (m >= MIN_STEP_SWITCH) ? MIN_STEP_LONG : MIN_STEP_SHORT;
  } else {
    m -= (m > MIN_STEP_SWITCH) ? MIN_STEP_LONG : MIN_STEP_SHORT;
  }
  // C++: constrain() keeps the result inside the allowed range, so tapping
  //      past either end just stops there. The (long) casts make all three
  //      values the same type.
  setDuration(constrain(m, (long)MIN_STEP_SHORT, (long)MAX_DURATION_MIN));
}

// A button was tapped. "id" is the button's component id (see ID_... above).
void handleTouch(uint8_t id) {
  // C++: case labels can be named constants, as long as they are "const"
  //      values the compiler knows in advance.
  switch (id) {
    case ID_TEMP_DN:
      setSetpoint(constrain(setpointF - TEMP_STEP_F, MIN_SETPOINT_F, MAX_SAFE_F - 25));
      break;
    case ID_TEMP_UP:
      setSetpoint(constrain(setpointF + TEMP_STEP_F, MIN_SETPOINT_F, MAX_SAFE_F - 25));
      break;
    case ID_MIN_DN: stepDuration(false); break;
    case ID_MIN_UP: stepDuration(true);  break;
    case ID_START:  startRun(); break;
    case ID_STOP:   stopRun();  break;
    default:
      // A component we don't know. Usually means the IDs don't match the
      // ID_... constants: check each button's "id" in Nextion Editor.
      Serial.print(F("Touch from unknown component id "));
      Serial.println(id);
      return;
  }
  nexUpdateNow = true;  // show the result on the screen straight away
}

// Act on one complete message (the bytes before the FF FF FF).
// C++: "const uint8_t *msg" is a pointer to bytes this function promises not
//      to change (const). The length is passed separately, because raw bytes
//      have no '\0' marking the end like text does.
void handleNexMessage(const uint8_t *msg, uint8_t len) {
  if (len == 4 && msg[0] == NEX_TOUCH) {
    markScreenAlive();  // before handleTouch(): Start checks the link
    // msg[1] = page, msg[2] = component id, msg[3] = 00 released / 01 pressed
    if (msg[3] == 0x00) {
      handleTouch(msg[2]);
    } else {
      Serial.println(F("Touch PRESS ignored: tick 'Send Component ID' under "
                       "Touch Release Event only"));
    }
  } else if (len == 2 && msg[0] == NEX_PAGE) {
    markScreenAlive();  // the reply to our "sendme" ping
  } else if (len == 1 && msg[0] == NEX_READY) {
    // The screen has just started (or restarted): set it up, then redraw.
    nexSend(F("bkcmd=0"));
    nexUpdateNow = true;
  } else {
    badFrames++;  // a message we don't use, or a garbled one: ignore it
  }
}

// Read bytes from the screen and split them into messages at each FF FF FF.
void handleNextion() {
  while (nex.available()) {
    uint8_t b = nex.read();

    if (b == 0xFF) {
      nexFFCount++;
      if (nexFFCount == 3) {
        // End marker reached: use the message, unless it was damaged.
        if (nexBad) badFrames++;
        else if (nexLen > 0) handleNexMessage(nexBuf, nexLen);
        nexLen = 0;
        nexFFCount = 0;
        nexBad = false;
      }
      // C++: "continue" skips the rest of this pass of the while loop and
      //      goes straight on to the next byte.
      continue;
    }

    // A normal (non-FF) byte. None of the messages we use contain FF as data,
    // so one or two FFs followed by something else means bytes were lost.
    // Too many bytes for any message we use also means it's garbled.
    if (nexFFCount > 0 || nexLen >= sizeof(nexBuf)) nexBad = true;
    nexFFCount = 0;
    if (!nexBad) nexBuf[nexLen++] = b;
  }
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
  // Make the relay pins outputs and set them OFF straight away. (On the ESP32
  // the pin can only be written after pinMode; the moment in between lasts
  // microseconds, far too short for a relay to react.)
  for (uint8_t i = 0; i < NUM_HEATERS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelay(i, false);
  }

  // C++: INPUT_PULLUP turns on a resistor inside the chip that pulls the pin
  //      up to 3.3 V. The pin reads HIGH unless something connects it to GND.
  pinMode(PIN_ESTOP, INPUT_PULLUP);

  Serial.begin(9600);   // USB serial to the computer
  // Serial2 on the ESP32 can use almost any pins, so we pass them in:
  // speed, data format (8 data bits, no parity, 1 stop bit), RX pin, TX pin.
  nex.begin(NEX_BAUD, SERIAL_8N1, PIN_NEX_RX, PIN_NEX_TX);
  delay(500);  // let the thermocouple board and screen start up
#if USE_MAX31855
  if (!thermocouple.begin()) Serial.println(F("MAX31855 init failed"));
#endif
  nex.print(F("bkcmd=0"));  // stop the screen replying to every instruction
  nexEnd();

  Serial.println(F("Heater controller started - press Start (or send G) to run"));
  printSettings();

  // The watchdog is a timer that restarts the ESP32 if loop() doesn't "feed"
  // it (esp_task_wdt_reset) within 3 seconds. If the code ever gets stuck,
  // the board restarts, and setup() turns all heaters off again.
  //
  // C++: #if checks the ESP32 board package version at compile time, because
  //      the watchdog setup function changed in version 3. This way the
  //      sketch compiles on both older (2.x) and newer (3.x) versions.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 3000,
    .idle_core_mask = 0,      // don't also watch the idle tasks
    .trigger_panic = true,    // restart the chip when it times out
  };
  esp_task_wdt_reconfigure(&wdtConfig);
#else
  esp_task_wdt_init(3, true);  // 3 seconds, restart the chip when it times out
#endif
  esp_task_wdt_add(NULL);  // NULL = watch the task running this code (loop)
}

// C++: loop() runs over and over, thousands of times per second.
//
// Notice there is no delay() in here. Instead, each job checks "has enough
// time passed since I last ran?" using millis(). This lets several jobs run at
// different rates without blocking each other, a common Arduino pattern
// sometimes called "blink without delay".
void loop() {
  esp_task_wdt_reset();  // feed the watchdog: "the program is still running fine"
  unsigned long now = millis();  // read the clock once and reuse it

  // Every pass: E-stop first, so it is never delayed by anything else.
  checkEstop(now);

  // Every pass: check for typed commands and screen commands.
  handleSerial();
  handleNextion();
  checkScreenLink();

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
      if (isRunning()) checkOvershoot(currentTempF);
      // Checked again: checkOvershoot() may have just faulted.
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
