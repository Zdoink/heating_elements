// Relay test - ESP32
// Turns the relays ON for 1.5 seconds, then OFF for 1.5 seconds, forever.
// Use it to check relay wiring. Unplug the heaters before running it.
//
// Wiring: relay inputs IN1..IN6 -> GPIO 25, 26, 27, 32, 33, 4 (same pins as
// heater_controller_esp32). The relays must switch reliably from 3.3 V:
// see the README's "Relays must work from 3.3 V" section.
// Serial Monitor at 9600 baud shows ON/OFF as the relays switch.

const uint8_t NUM_RELAYS = 6;
const uint8_t RELAY_PINS[NUM_RELAYS] = {25, 26, 27, 32, 33, 4};
// Most common relay modules turn ON when the input is LOW. If the relays are
// on when the Serial Monitor says OFF, set this to false (also in
// heater_controller_esp32). Solid-state relays usually need false.
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
  // On the ESP32 a pin can only be written after pinMode, so enable the pins
  // and set them OFF straight away.
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
