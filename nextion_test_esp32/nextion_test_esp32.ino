// Nextion link test - ESP32
//
// Checks the ESP32 <-> Nextion connection on its own, before running the full
// heater controller. It never touches the relay pins, so no heater can turn on.
//
// What it does:
//   - Every second, writes a counter into tTemp, "TEST" into tStatus, blinks
//     tBeat, and sends "sendme" (the screen should reply).
//   - Prints every message the screen sends, in hex, with what it means.
//
// Wiring (same as heater_controller_esp32):
//   Nextion TX (blue) -> GPIO16, Nextion RX (yellow) -> GPIO17,
//   Nextion GND -> ESP32 GND, Nextion 5V from its own 5 V supply
//
// Open the Serial Monitor at 115200 baud, then tap each button on the screen.
// Expected output when you tap Start (id 5):
//   65 00 05 00 | TOUCH page 0 id 5 RELEASED -> bStart

HardwareSerial &nex = Serial2;
const uint8_t PIN_NEX_RX = 16;
const uint8_t PIN_NEX_TX = 17;
const long NEX_BAUD = 9600;

// Same button order as NEXTION_ESP32.md
// C++: An array of C strings: each entry is a pointer to some text.
//      Index 0 is the page itself, so buttons 1-6 line up with their ids.
const char *BUTTON_NAMES[] = {"page0", "bTempDn", "bTempUp", "bMinDn",
                              "bMinUp", "bStart", "bStop"};
// C++: sizeof(array) / sizeof(one element) = number of entries (7 here).
const uint8_t NUM_BUTTON_NAMES = sizeof(BUTTON_NAMES) / sizeof(BUTTON_NAMES[0]);

uint8_t msg[16];
uint8_t msgLen = 0;
uint8_t ffCount = 0;
unsigned long lastSendMs = 0;
unsigned long counter = 0;
unsigned long replies = 0;   // sendme replies received
unsigned long pings = 0;     // sendme pings sent

void nexEnd() {
  nex.write(0xFF); nex.write(0xFF); nex.write(0xFF);
}

// Print one byte as two hex digits, e.g. 5 -> "05"
void printHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);  // C++: the second argument picks the number base
}

void describeMessage() {
  for (uint8_t i = 0; i < msgLen; i++) {
    printHex(msg[i]);
    Serial.print(' ');
  }
  Serial.print(F("| "));

  if (msgLen == 4 && msg[0] == 0x65) {
    uint8_t id = msg[2];
    Serial.print(F("TOUCH page "));
    Serial.print(msg[1]);
    Serial.print(F(" id "));
    Serial.print(id);
    Serial.print(msg[3] == 0x00 ? F(" RELEASED") : F(" PRESSED (should be Release only!)"));
    Serial.print(F(" -> "));
    if (id < NUM_BUTTON_NAMES) Serial.println(BUTTON_NAMES[id]);
    else Serial.println(F("unknown id: check the button order in Nextion Editor"));
  } else if (msgLen == 2 && msg[0] == 0x66) {
    replies++;
    Serial.print(F("SENDME reply: on page "));
    Serial.println(msg[1]);
  } else if (msgLen == 1 && msg[0] == 0x88) {
    Serial.println(F("SCREEN STARTED UP"));
  } else if (msgLen == 3 && msg[0] == 0x00) {
    Serial.println(F("screen power-on message (normal)"));
  } else if (msgLen == 1 && msg[0] == 0x1A) {
    Serial.println(F("INVALID VARIABLE: a component name doesn't match (tTemp/tStatus/tBeat?)"));
  } else if (msgLen == 1 && msg[0] == 0x00) {
    Serial.println(F("INVALID INSTRUCTION"));
  } else {
    Serial.println(F("other / garbled message"));
  }
}

void setup() {
  Serial.begin(115200);
  nex.begin(NEX_BAUD, SERIAL_8N1, PIN_NEX_RX, PIN_NEX_TX);
  delay(500);
  // bkcmd=2: during this test, ask the screen to report failed instructions,
  // so a wrong component name shows up as "INVALID VARIABLE" below.
  // (The real controller uses bkcmd=0, no reports.)
  nex.print("bkcmd=2");
  nexEnd();
  Serial.println(F("Nextion link test. Tap each screen button and watch here."));
}

void loop() {
  // ---- Read and split messages at FF FF FF ----
  while (nex.available()) {
    uint8_t b = nex.read();
    if (b == 0xFF) {
      if (++ffCount == 3) {
        if (msgLen > 0) describeMessage();
        msgLen = 0;
        ffCount = 0;
      }
      continue;
    }
    // FF bytes that weren't the end marker were data: keep them
    while (ffCount > 0 && msgLen < sizeof(msg)) { msg[msgLen++] = 0xFF; ffCount--; }
    ffCount = 0;
    if (msgLen < sizeof(msg)) msg[msgLen++] = b;
  }

  // ---- Once a second: write to the screen and ping it ----
  if (millis() - lastSendMs >= 1000) {
    lastSendMs = millis();
    counter++;

    nex.print("tTemp.txt=\""); nex.print(counter); nex.print('"'); nexEnd();
    nex.print("tStatus.txt=\"TEST\""); nexEnd();
    nex.print("tBeat.txt=\""); nex.print(counter % 2 ? "*" : ""); nex.print('"'); nexEnd();
    nex.print("sendme"); nexEnd();
    pings++;

    // Every 10 s, a summary: replies should keep up with pings
    if (counter % 10 == 0) {
      Serial.print(F("-- link check: "));
      Serial.print(replies);
      Serial.print(F(" replies to "));
      Serial.print(pings);
      Serial.println(replies + 2 >= pings ? F(" pings: OK") : F(" pings: NO / FEW REPLIES - check blue wire to GPIO16"));
    }
  }
}
