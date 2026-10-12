// Relay test - Arduino Uno R3 or ESP32
// Turns the relays ON for 1.5 seconds, then OFF for 1.5 seconds, forever.
// Use it to check relay wiring. Unplug the heaters before running it.
//
// Wiring (the right pins are picked automatically for the board you select):
//   Uno:   relay inputs IN1..IN6 -> D2..D7, relay module VCC/GND -> 5V/GND
//   ESP32: relay inputs IN1..IN6 -> GPIO 25, 26, 27, 32, 33, 4
//          (relays must switch from 3.3 V - see heater_controller_esp32.ino)
// Serial Monitor at 9600 baud shows ON/OFF as the relays switch.

const uint8_t NUM_RELAYS = 6;
// C++: ESP32 is defined automatically when an ESP32 board is selected, so the
//      same sketch uses the right pins on either board.
#if defined(ESP32)
const uint8_t RELAY_PINS[NUM_RELAYS] = {25, 26, 27, 32, 33, 4};
#else
const uint8_t RELAY_PINS[NUM_RELAYS] = {2, 3, 4, 5, 6, 7};
#endif
// Most common relay modules turn ON when the input is LOW. If the relays are
// on when the Serial Monitor says OFF, set this to false.
const bool RELAY_ACTIVE_LOW = true;

const unsigned long TOGGLE_MS = 1500;

bool relaysOn = false;
unsigned long lastToggleMs = 0;

void writeRelays(bool on) {
  for (uint8_t i = 0; i < NUM_RELAYS; i++) {
    digitalWrite(RELAY_PINS[i], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
  }
}

void setup() {
#if !defined(ESP32)
  // Uno: set OFF before enabling the pins as outputs so nothing blips on at
  // boot. (The ESP32 can only write a pin after pinMode.)
  writeRelays(false);
#endif
  for (uint8_t i = 0; i < NUM_RELAYS; i++) pinMode(RELAY_PINS[i], OUTPUT);
  writeRelays(false);

  Serial.begin(9600);
  Serial.println(F("Relay test: toggling every 1.5 s"));
}

void loop() {
  if (millis() - lastToggleMs >= TOGGLE_MS) {
    lastToggleMs = millis();
    relaysOn = !relaysOn;
    writeRelays(relaysOn);
    Serial.println(relaysOn ? F("ON") : F("OFF"));
  }
}
