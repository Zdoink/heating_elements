// Relay test - Arduino Uno R3
// Turns the relays ON for 1.5 seconds, then OFF for 1.5 seconds, forever.
// Use it to check relay wiring. Unplug the heaters before running it.
//
// Wiring: relay inputs IN1..IN6 -> D2..D7, relay module VCC/GND -> 5V/GND
// Serial Monitor at 9600 baud shows ON/OFF as the relays switch.

const uint8_t NUM_RELAYS = 6;
const uint8_t RELAY_PINS[NUM_RELAYS] = {2, 3, 4, 5, 6, 7};
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
  // Set OFF before enabling the pins as outputs so nothing blips on at boot
  writeRelays(false);
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
