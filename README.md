# Heating Elements

**Repository:** https://github.com/Zdoink/heating_elements

Temperature-controlled heating enclosure built around an **ESP32**.
A **Type K thermocouple** measures the enclosure temperature, and the ESP32
uses **PID control** to drive **6 heating elements** through relays, holding a
steady temperature (default **500 °F**) for a set time. A **Nextion NX4827P043
touch screen** (4.3", 480 × 272) sets the temperature and time and starts and
stops each run.

## How it works

**Reading the temperature.** A Type K thermocouple outputs a very small analog
voltage (about 22 µV/°F) that the ESP32 cannot read directly. A **MAX31855** or
**MAX6675** amplifier board converts it to a digital reading that the ESP32
reads over SPI. Readings are taken every 250 ms and averaged each second to
smooth out noise.

**Steady control (PID).** Once per second the PID controller compares the
temperature to the setpoint and calculates how much heater power is needed,
from 0–100 %.

**Staged heaters.** That power level is spread across the 6 elements instead of
switching them all together:

| PID output | Heaters 1–3 | Heater 4 | Heaters 5–6 |
|---|---|---|---|
| 50 % | ON | OFF | OFF |
| 58 % | ON | ON for 50 % of each 10 s window | OFF |

Only one relay ever cycles at a time, which gives fine control and keeps relay
wear and current surges low.

**Runs and the hold timer.** The heaters stay off until a run is started:

```
IDLE --Start--> HEATING --within 5 °F of target--> HOLDING --time up--> DONE
```

- **HEATING:** warming up to the target temperature.
- **HOLDING:** at temperature; the hold timer counts down.
- **DONE:** time is up and the heaters are off. Press Start to run again.
- **Stop** turns the heaters off immediately from any state.

**The screen.** The Nextion holds **no code**. It shows what the ESP32 sends it
and reports which button was tapped. All the logic is in the ESP32. See
[NEXTION_ESP32.md](NEXTION_ESP32.md).

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | Classic ESP32 (ESP32-WROOM / "ESP32 Dev Module") |
| MAX31855 **or** MAX6675 board | Thermocouple amplifier with SPI output; powered from 3.3 V |
| Type K thermocouple | Rated well above 500 °F; yellow = +, red = − |
| 6 relays that switch from **3.3 V** | Solid-state relays with 3–32 V DC input recommended; each rated for one heater's voltage and current. See [Relays](#relays-must-work-from-33-v) |
| 6 heating elements | |
| Nextion NX4827P043 | 4.3", 480 × 272 touch screen; see [NEXTION_ESP32.md](NEXTION_ESP32.md) |
| 5 V power supply (≥1 A) | Powers the Nextion screen (and the ESP32 through its 5V/VIN pin if you like) |
| E-stop button | Mushroom-head, latching, with a **normally-closed (NC)** contact; ideally a second NC contact to cut heater power directly |
| Thermal fuse / high-limit thermostat | Independent hardware backup cutoff |

> ⚠️ **The ESP32 is a 3.3 V board.** Its pins must never receive 5 V.
> Power the thermocouple board from **3V3**, not 5V. Otherwise its data line
> sends 5 V into the ESP32.

## Wiring

| From | To (ESP32 GPIO) |
|---|---|
| Thermocouple board VCC | **3V3** |
| Thermocouple board GND | GND |
| Thermocouple board SCK / CLK | GPIO18 |
| Thermocouple board SO / DO | GPIO19 |
| Thermocouple board CS | GPIO23 |
| Relay 1 – 6 inputs | GPIO25, 26, 27, 32, 33, 4 |
| Nextion TX (blue) | GPIO16 |
| Nextion RX (yellow) | GPIO17 |
| Nextion GND (black) | GND (and the 5 V supply's GND) |
| Nextion 5V (red) | Separate 5 V supply |
| E-stop NC contact | Between GPIO13 and GND |

- Thermocouple leads go into the amplifier board's screw terminal (yellow to +,
  red to −).
- **Pin choice:** these pins avoid the ESP32's boot-sensitive pins
  (GPIO 0, 2, 5, 12, 15), so no heater flicks on at power-up.
- The Nextion must share a ground with the ESP32 or the two can't communicate.
  Its serial signals are 3.3 V level, so TX/RX connect directly.
- **E-stop:** use the button's **normally-closed** contact, so a pressed button
  *or* a broken wire stops the heaters. No E-stop fitted yet? Put a wire jumper
  from **GPIO13 to GND**, or the controller will go straight to FAULT at power-up.
  For a long E-stop cable, add a 10 kΩ resistor from GPIO13 to **3V3** to
  prevent false trips from electrical noise.
- For a true emergency stop, also wire a second NC contact on the E-stop so it
  cuts power to the heater relays or contactor directly. Then it works even if
  the ESP32 doesn't.

### Relays must work from 3.3 V

A 5 V relay module that turns on with a LOW input may not fully turn **off**
from the ESP32's 3.3 V "high", which could leave a heater on. Use one of these:
- **Solid-state relays with a 3–32 V DC input:** recommended. Set
  `RELAY_ACTIVE_LOW = false` in the sketch.
- **An optocoupler relay module with a JD-VCC jumper:** remove the jumper, power
  VCC from 3V3 and JD-VCC from 5V.
- **A relay module sold as 3.3 V compatible.**

Check them with `relay_test.ino` before connecting any heaters (see
[Bring-up order](#bring-up-order)).

## Uploading the code to the ESP32

> **Do this with the heaters unplugged.** Only the ESP32, thermocouple board,
> relays and screen need to be connected while you upload and test.

### 1. Install the Arduino IDE and ESP32 support

1. Download and install **Arduino IDE 2** from
   [arduino.cc/en/software](https://www.arduino.cc/en/software).
2. Open **Tools → Board → Boards Manager**, search for **esp32** and install
   **esp32 by Espressif Systems**.

### 2. Download this project

Either:

- **Download ZIP:** on [github.com/Zdoink/heating_elements](https://github.com/Zdoink/heating_elements),
  click the green **Code** button, then **Download ZIP**, and unzip it.
- **Or clone it with Git:**
  ```
  git clone https://github.com/Zdoink/heating_elements.git
  ```

The Arduino IDE requires each sketch's folder name to match its `.ino` file
name (e.g. `heater_controller_esp32/heater_controller_esp32.ino`), so keep the
folder names as they are.

### 3. Install the thermocouple library

1. Open the **Library Manager** (icon on the left, or
   **Sketch → Include Library → Manage Libraries…**).
2. Install the library for your board:
   - MAX31855 board: **Adafruit MAX31855 library**
   - MAX6675 board: **MAX6675 library** by Adafruit
3. If asked to install dependencies (such as **Adafruit BusIO**), click **Install All**.

No other libraries are needed. The ESP32's built-in hardware serial port talks
to the screen.

### 4. Open and configure the sketch

1. **File → Open…** and select `heater_controller_esp32/heater_controller_esp32.ino`.
2. Near the top of the sketch, set the thermocouple board type:
   ```cpp
   #define USE_MAX31855 1  // 1 = MAX31855 board, 0 = MAX6675 board
   ```
   Check the part number printed on the chip on your thermocouple board.
3. If you use solid-state relays (active-HIGH), set `RELAY_ACTIVE_LOW = false`.

### 5. Connect and select the board

1. Plug the ESP32 into your computer with a **data** USB cable (some cables
   only charge).
2. **Tools → Board → esp32 → ESP32 Dev Module** (or your exact board).
3. **Tools → Port**, then pick the ESP32's COM port. Unsure which? Unplug the
   board, see which port disappears, and plug it back in.
   - No port shown? Most ESP32 boards use a **CP2102** or **CH340** USB chip.
     Install that driver, then reconnect.

### 6. Verify and upload

1. Click **Verify** (✓) to compile.
2. Click **Upload** (→). If it stalls at `Connecting...`, hold the board's
   **BOOT** button until the upload starts, then release it.

### 7. Check that it is running

1. Open **Tools → Serial Monitor** at **9600 baud**. (A few garbled characters
   right at power-up are the ESP32's boot messages and are normal.) You should see:
   ```
   Heater controller started - press Start (or send G) to run
   Setpoint=500.0F Hold=60min Kp=3.000 Ki=0.0200 Kd=0.000 State=IDLE BadFrames=0
   Temp_F:72.5 Setpoint_F:500.0 Output_pct:0.0 State:0
   ```
2. Hold the thermocouple tip in your hand. `Temp_F` should rise.
3. **Tools → Serial Plotter** (close the Serial Monitor first) shows the
   temperature, setpoint and output as a live graph.

### Upload troubleshooting

| Problem | Fix |
|---|---|
| `Adafruit_MAX31855.h` or `max6675.h`: No such file or directory | The library from step 3 isn't installed |
| `Failed to connect to ESP32: Timed out waiting for packet header` | Hold the **BOOT** button while uploading; check the port and cable |
| No port in Tools → Port | Try another USB cable, or install the CP2102 / CH340 driver |
| Board keeps restarting (`rst:` messages repeat) | Usually a weak USB power supply; try another port or a powered hub |
| Readings show `Thermocouple fault OPEN` | Thermocouple not connected, or loose in the screw terminal |
| Readings are wildly wrong or always 0 | Thermocouple board powered from 5V instead of 3V3, or CLK/DO/CS wires swapped |
| Temperature goes **down** when heated | Thermocouple wires are reversed; swap yellow and red |
| Random characters in the Serial Monitor | Baud rate is not set to 9600 |
| `FAULT: E-stop pressed or E-stop wire broken` right at power-up | E-stop pressed, wired to its NO contact instead of NC, or not fitted: jumper GPIO13 to GND |
| `Can't start: no link to the screen` | Screen not connected or not replying; run the link test (see below). For USB-only testing, see "Testing without the screen" |
| `Touch from unknown component id …` | Screen button ids don't match; see [NEXTION_ESP32.md](NEXTION_ESP32.md) troubleshooting |

## Bring-up order

Do these in order, **with the heaters unplugged**:

1. **Relays:** upload `relay_test/relay_test.ino`. All six relays should click
   on and off every 1.5 s, matching ON/OFF in the Serial Monitor (9600 baud).
   If they're on when it says OFF, set `RELAY_ACTIVE_LOW = false` in both sketches.
2. **Screen:** build and load the screen ([NEXTION_ESP32.md](NEXTION_ESP32.md)),
   then upload `nextion_test_esp32/nextion_test_esp32.ino` and check the link,
   item names and button ids (Serial Monitor at 115200 baud).
3. **Controller:** upload `heater_controller_esp32/heater_controller_esp32.ino`.
   Press **Start** on the screen: the relays should click on, because room temperature
   is far below the setpoint. Press **Stop** and they should click off.
4. **Safety shutoffs:** test link loss and the E-stop as described at the end
   of [NEXTION_ESP32.md](NEXTION_ESP32.md).
5. Only then connect the heaters, and stay with the enclosure for the first runs.

**Testing without the screen:** a run can't start while there is no screen
link (the Serial Monitor shows `Can't start: no link to the screen`). For bench
testing over USB only, temporarily set `REQUIRE_SCREEN_LINK = false`, upload,
and use `G` and `X` in the Serial Monitor. **Set it back to `true` before
real use.**

## Using the screen

| Control | What it does |
|---|---|
| Temp + / − | Change the target in 5 °F steps (100–535 °F) |
| Time + / − | Change the hold time: 5-minute steps up to 2 hours, 15-minute steps above (5–1440 min) |
| Start | Begin a run: heat up, then hold for the set time |
| Stop | Turn all heaters off immediately |

The screen shows the current temperature, status, time left in the hold,
heater power and any fault reason. Settings can be changed during a run. The
`*` next to the title blinks while the screen is connected to the ESP32.

### How the ESP32 and screen avoid communication errors

- **Hardware serial:** the ESP32's built-in serial port receives in the
  background while the program runs, so no bytes are dropped.
- **Strict message checks:** a button tap is only accepted if the message has
  exactly the right length and type. A garbled message is dropped, never
  mistaken for a different button. If a tap is lost, the number on screen
  doesn't change, so you can see it and tap again.
- **The ESP32 is the only source of truth:** the screen only displays what the
  ESP32 sends, so it always shows the settings actually in use. It's fully
  refreshed every second, so a corrupted update fixes itself.
- **Safe repeats:** a repeated Start doesn't restart the timer, and a repeated
  Stop does nothing extra.
- **Link check:** every second the ESP32 sends `sendme` and the screen replies
  by itself. If replies stop for 5 seconds during a run, the heaters shut off.
- **Screen restarts** are detected and the screen is redrawn immediately.

## Serial commands

Type into the Serial Monitor and press Enter. These work alongside the screen.
Changes last until the board is reset; copy the values you settle on into the sketch.

| Command | Example | Effect |
|---|---|---|
| `S<temp>` | `S500` | Set the setpoint in °F |
| `M<minutes>` | `M90` | Set the hold time in minutes |
| `G` | `G` | Start a run (needs the screen connected unless `REQUIRE_SCREEN_LINK = false`) |
| `X` | `X` | Stop: all heaters off |
| `P<value>` | `P3.0` | Set Kp |
| `I<value>` | `I0.02` | Set Ki |
| `D<value>` | `D10` | Set Kd |
| `?` | `?` | Print the current settings, state and count of unrecognised screen messages |

## Tuning the PID

The default gains are only a starting point. Every enclosure heats differently,
so they need tuning on yours. Start a long run (e.g. `M600`, then Start) and use
the Serial Plotter to watch the temperature.

1. Start with `I0` and `D0`. Raise `P` until the temperature settles close to
   500 °F with only a small, slow oscillation.
2. Raise `I` slowly (e.g. 0.005 → 0.01 → 0.02) until the temperature settles
   **on** 500 °F instead of slightly below it. If it starts oscillating, lower `I`.
3. If it overshoots after the warm-up or after the door is opened, add a little
   `D` (start around 5–20).
4. Put the final values into `Kp`, `Ki` and `Kd` in the sketch.

While tuning, any overshoot of more than 20 °F above the setpoint trips the
overshoot fault and shuts the heaters off. That's expected. Reset, lower `P`
or `I`, and try again. A well-tuned enclosure should stay well under 20 °F.

## Configuration

| Constant | Default | Meaning |
|---|---|---|
| `USE_MAX31855` | 1 | 1 = MAX31855 board, 0 = MAX6675 board |
| `setpointF` | 500.0 | Target temperature at power-up (°F) |
| `durationMin` | 60 | Hold time at power-up (minutes) |
| `READY_BAND_F` | 5.0 | Hold timer starts once within this many °F of the target |
| `Kp`, `Ki`, `Kd` | 3.0, 0.02, 0.0 | PID gains (output in % of total power) |
| `INTEGRAL_BAND_F` | 40.0 | Integral only builds within ± this many °F of the setpoint, to prevent warm-up overshoot |
| `RELAY_PINS` | GPIO 25, 26, 27, 32, 33, 4 | Relay input pins for heaters 1–6 |
| `RELAY_ACTIVE_LOW` | true | `true` for relay modules that switch on with a LOW input; `false` for SSRs |
| `WINDOW_MS` | 10000 | Cycle time for the one switching relay (can be ~2000 with SSRs) |
| `MIN_PULSE_MS` | 1000 | Shortest relay on/off pulse allowed |
| `PIN_TC_CLK` / `PIN_TC_DO` / `PIN_TC_CS` | 18 / 19 / 23 | Thermocouple board pins |
| `PIN_NEX_RX` / `PIN_NEX_TX` | 16 / 17 | Nextion serial pins |
| `NEX_BAUD` | 9600 | Nextion serial speed (must match the screen) |
| `ID_TEMP_DN` … `ID_STOP` | 1 … 6 | Screen button component ids |
| `TEMP_STEP_F` | 5.0 | Temperature change per tap |
| `MIN_STEP_SHORT` / `MIN_STEP_LONG` / `MIN_STEP_SWITCH` | 5 / 15 / 120 | Hold-time step per tap, and where the bigger step starts (minutes) |
| `MAX_SAFE_F` | 560.0 | Over-temperature cutoff (°F) |
| `MAX_BAD_READS` | 4 | Consecutive sensor faults before shutdown |
| `RUNAWAY_TIME_MS` / `RUNAWAY_MIN_RISE_F` | 10 min / 15 °F | Thermal runaway detection |
| `OVERSHOOT_LIMIT_F` | 20.0 | Fault if the temperature goes this far above the setpoint during a run |
| `PIN_ESTOP` | 13 | E-stop input (NC contact to GND) |
| `ESTOP_DEBOUNCE_MS` | 20 | E-stop must read open this long before tripping (filters noise) |
| `REQUIRE_SCREEN_LINK` | true | Shut off if the screen link is lost during a run; `false` only for USB bench testing |
| `SCREEN_TIMEOUT_MS` | 5000 | How long without a reply from the screen before shutting off |

## Safety features

- **Off until started:** heaters stay off at power-up and after a reset until
  Start is pressed.
- **Stop:** turns all heaters off immediately.
- **Run complete:** heaters turn off automatically when the hold time ends.

These faults **latch every heater off** and show **FAULT** (with the reason on
the screen) until the board is reset:

- **E-stop:** the E-stop button is pressed, or its wire is broken. This is
  checked on every pass of the program and reacts within about 20 ms.
- **Overshoot:** during a run, the temperature goes more than 20 °F above the
  setpoint (e.g. 520 °F with a 500 °F setpoint). If you lower the setpoint
  mid-run, this check waits until the enclosure has cooled to within 20 °F
  of the new setpoint before it applies.
- **Screen link lost:** during a run, no reply from the screen for 5 seconds.
- **Over-temperature:** the temperature reaches `MAX_SAFE_F` (560 °F), whatever the setpoint.
- **Sensor fault:** the thermocouple is open or shorted for about 1 second.
- **Thermal runaway:** the heaters run near full power for 10 minutes without
  the temperature rising 15 °F. This usually means the thermocouple has come
  loose or out of the enclosure.

Two more protections:

- **Watchdog:** if the code hangs, the ESP32 restarts within 3 seconds, which
  turns the heaters off.
- **Safe startup:** the relay pins are set to OFF as soon as they're enabled,
  and the chosen pins don't glitch during boot.

The screen never controls the heaters directly. It only reports taps. If it
disconnects mid-run, the link check shuts the heaters off.

> ⚠️ **Mains voltage warning:** Never drive a heater directly from an ESP32
> pin. Use relays rated for each heater's voltage and current, protect each
> circuit with a correctly sized breaker or fuse, and add an independent
> high-limit thermostat or thermal fuse so the heaters shut off even if the
> ESP32 or a relay fails (relays can fail welded ON). Keep the thermocouple
> wiring away from mains wiring to avoid noisy readings.

## Relay choice

PID control switches one relay every few seconds. Mechanical relays are
typically rated for about 100,000 operations, so the switching relay will wear
out over time. **Solid-state relays (SSRs)** with a 3–32 V DC input are
recommended for all six heaters: they last far longer and switch directly
from the ESP32's 3.3 V. With SSRs, `WINDOW_MS` can be lowered to about 2000 for
even steadier control.

## Project structure

```
heating_elements/
├── README.md
├── NEXTION_ESP32.md            Nextion screen build guide (NX4827P043, no code on the screen)
├── images/
│   └── nextion_esp32_layout.svg  Screen layout picture used in NEXTION_ESP32.md
├── heater_controller_esp32/
│   └── heater_controller_esp32.ino   The heater controller
├── nextion_test_esp32/
│   └── nextion_test_esp32.ino  ESP32 ↔ Nextion link test (no relays used)
└── relay_test/
    └── relay_test.ino          Relay wiring test: all relays on/off every 1.5 s
```

## Roadmap

- [x] Nextion touch screen for setpoint, hold time, start and stop
- [ ] Multi-step profiles (e.g. 300 °F for 30 min, then 500 °F for 60 min)
- [ ] Save settings and tuned PID values to flash, so they survive a restart
- [ ] Wi-Fi monitoring from a phone
- [ ] Rotate which heater does the switching to spread relay wear
