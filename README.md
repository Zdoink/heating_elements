# Heating Elements

**Repository:** https://github.com/Zdoink/heating_elements

Temperature-controlled heating enclosure built around an **Arduino Uno R3**.
A **Type K thermocouple** measures the enclosure temperature, and the Arduino
uses **PID control** to drive **6 heating elements** through relays, holding a
steady temperature (default **500 °F**) for a set time. A **Nextion touch
screen** sets the temperature and time and starts and stops each run.

## How it works

**Reading the temperature.** A Type K thermocouple outputs a very small analog
voltage (about 22 µV/°F) that the Uno cannot read directly. A **MAX31855** or
**MAX6675** amplifier board converts it to a digital reading that the Uno reads
over SPI. Readings are taken every 250 ms and averaged each second to smooth out
noise.

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

## Hardware

| Part | Notes |
|---|---|
| Arduino Uno R3 | Controller |
| MAX31855 **or** MAX6675 board | Thermocouple amplifier with SPI output |
| Type K thermocouple | Rated well above 500 °F; yellow = +, red = − |
| 6-channel relay module or 6 SSRs | Each rated for one heater's voltage and current |
| 6 heating elements | |
| Nextion touch screen | Any Nextion model; see [NEXTION.md](NEXTION.md) |
| 5 V power supply (≥1 A) | Powers the Nextion screen |
| E-stop button | Mushroom-head, latching, with a **normally-closed (NC)** contact; ideally a second NC contact to cut heater power directly |
| Thermal fuse / high-limit thermostat | Independent hardware backup cutoff |

## Wiring

| From | To (Uno) |
|---|---|
| Thermocouple board VCC | 5V |
| Thermocouple board GND | GND |
| Thermocouple board SCK / CLK | D13 |
| Thermocouple board SO / DO | D12 |
| Thermocouple board CS | D10 |
| Relay 1 – 6 inputs (IN1–IN6) | D2, D3, D4, D5, D6, D7 |
| Relay module VCC / GND | 5V / GND |
| Nextion TX (blue) | D8 |
| Nextion RX (yellow) | D9 |
| Nextion GND (black) | GND (and the 5 V supply's GND) |
| Nextion 5V (red) | Separate 5 V supply, **not** the Uno's 5V pin |
| E-stop NC contact | Between A0 and GND |

- Thermocouple leads go into the amplifier board's screw terminal (yellow to +,
  red to −).
- If the relay module has a JD-VCC jumper, a separate 5 V supply for the relay
  coils is recommended because 6 coils draw a lot of current.
- The Nextion must share a ground with the Uno or the two can't communicate.
- D8 and D9 are fixed by the AltSoftSerial library and can't be changed.
- **E-stop:** use the button's **normally-closed** contact, so a pressed button
  *or* a broken wire stops the heaters. No E-stop fitted yet? Put a wire jumper
  from **A0 to GND**, or the controller will go straight to FAULT at power-up.
- For a true emergency stop, also wire a second NC contact on the E-stop so it
  cuts power to the heater relays or contactor directly. Then it works even if
  the Arduino doesn't.

## Uploading the code to the Arduino

> **Do this with the heaters unplugged.** Only the Arduino, thermocouple board,
> relay module and screen need to be connected while you upload and test.

### 1. Install the Arduino IDE

Download and install **Arduino IDE 2** from
[arduino.cc/en/software](https://www.arduino.cc/en/software). Accept the driver
install prompts during setup.

### 2. Download this project

Either:

- **Download ZIP:** on [github.com/Zdoink/heating_elements](https://github.com/Zdoink/heating_elements),
  click the green **Code** button, then **Download ZIP**, and unzip it.
- **Or clone it with Git:**
  ```
  git clone https://github.com/Zdoink/heating_elements.git
  ```

You should have a folder containing:

```
heater_controller/
└── heater_controller.ino
```

The Arduino IDE requires the folder name to match the `.ino` file name, so keep
the `heater_controller` folder name as it is.

### 3. Install the libraries

1. In the Arduino IDE, click the **Library Manager** icon on the left (or
   **Sketch → Include Library → Manage Libraries…**).
2. Search for and install:
   - **AltSoftSerial** by Paul Stoffregen (for the Nextion screen)
   - MAX31855 board: **Adafruit MAX31855 library**, or
     MAX6675 board: **MAX6675 library** by Adafruit
3. If asked to install dependencies (such as **Adafruit BusIO**), click **Install All**.

### 4. Open and configure the sketch

1. **File → Open…** and select `heater_controller/heater_controller.ino`.
2. Near the top of the sketch, set the board type:
   ```cpp
   #define USE_MAX31855 1  // 1 = MAX31855 board, 0 = MAX6675 board
   ```
   Check the part number printed on the chip on your thermocouple board.

### 5. Connect the Arduino and select it

1. Plug the Uno into your computer with a USB A-to-B cable.
2. **Tools → Board → Arduino AVR Boards → Arduino Uno**.
3. **Tools → Port**, then pick the COM port labeled **Arduino Uno**. If you are
   unsure which one it is, unplug the Uno, check which port disappears, and
   plug it back in.
   - No port shown? Clone Unos often use a **CH340** USB chip that needs a
     driver. Install the CH340 driver, then reconnect the board.

### 6. Verify and upload

1. Click **Verify** (✓ checkmark) to compile. It should finish with
   "Done compiling".
2. Click **Upload** (→ arrow). The Uno's TX/RX lights blink, then the IDE
   shows "Done uploading".

### 7. Check that it is running

1. Open **Tools → Serial Monitor** and set the speed to **9600 baud**. You should see:
   ```
   Heater controller started - press Start (or send G) to run
   Setpoint=500.0F Hold=60min Kp=3.000 Ki=0.0200 Kd=0.000 State=IDLE BadFrames=0
   Temp_F:72.5 Setpoint_F:500.0 Output_pct:0.0 State:0
   ```
2. Hold the thermocouple tip in your hand. `Temp_F` should rise.
3. **Tools → Serial Plotter** (close the Serial Monitor first) shows the
   temperature, setpoint and output as a live graph.

### 8. Set up the screen

Build and load the screen file by following [NEXTION.md](NEXTION.md).

### Upload troubleshooting

| Problem | Fix |
|---|---|
| `AltSoftSerial.h`, `Adafruit_MAX31855.h` or `max6675.h`: No such file or directory | A library from step 3 isn't installed |
| `avrdude: stk500_recv(): programmer is not responding` | Wrong port or board selected; close the Serial Monitor and other programs using the port, then try again |
| No port in Tools → Port | Try another USB cable (some are charge-only) or install the CH340 driver |
| Readings show `Thermocouple fault OPEN` | Thermocouple not connected, or loose in the screw terminal |
| Temperature goes **down** when heated | Thermocouple wires are reversed; swap yellow and red |
| Random characters in the Serial Monitor | Baud rate is not set to 9600 |
| Screen shows **NO LINK** | Check TX/RX aren't swapped (Nextion TX → D8), and that the screen and Uno share GND |
| Serial Monitor shows `Rejected screen frame` | Check the `tm0` code in the Nextion project matches [NEXTION.md](NEXTION.md) |
| `FAULT: E-stop pressed or E-stop wire broken` right at power-up | E-stop pressed, wired to its NO contact instead of NC, or not fitted: jumper A0 to GND |
| `Can't start: no link to the screen` | Screen not connected or not sending the `<H>` heartbeat; check the `tm0` code. For USB-only testing see the first power-up check |

### First power-up check

With the heaters unplugged, the E-stop connected (or A0 jumpered to GND) and
the screen connected, press **Start** on the screen. All relays should click
**ON**, because room temperature is far below the setpoint. Press **Stop** and
they should all click **OFF**. If it's the other way round, your relay module
is active-HIGH: set `RELAY_ACTIVE_LOW = false`.

**Testing without the screen:** a run can't start while there is no screen
link (the Serial Monitor shows `Can't start: no link to the screen`). For bench
testing over USB only, temporarily set `REQUIRE_SCREEN_LINK = false`, upload,
and use `G` and `X` in the Serial Monitor. **Set it back to `true` before
real use.**

## Using the screen

| Control | What it does |
|---|---|
| Temp + / − | Change the target in 5 °F steps (100–535 °F) |
| Time + / − | Change the hold time in 5-minute steps (1–1440 min) |
| Start | Begin a run: heat up, then hold for the set time |
| Stop | Turn all heaters off immediately |

The screen shows the current temperature, status, time left in the hold, and
heater power. Settings can be changed during a run.

### How the Arduino and screen avoid communication errors

- **Full-duplex serial (AltSoftSerial):** the Arduino can receive screen
  commands while it is sending updates, so nothing is dropped.
- **Framed commands with a check value:** commands look like `<S500,29500>`.
  The value and check must add up to 30000, or the Arduino rejects the command.
- **Confirmed delivery:** the screen resends every command until the Arduino
  reports the new value back.
- **Safe repeats:** a repeated Start doesn't restart the timer, and a repeated
  Stop does nothing extra.
- **Self-correcting display:** the screen is fully refreshed every second.
- **Link check, both ways:** the screen shows **NO LINK** if the Arduino stops
  sending updates for about 3 seconds. The screen sends a heartbeat every 0.5 s,
  and if it goes quiet for 5 seconds during a run, the Arduino shuts the heaters off.

Full details are in [NEXTION.md](NEXTION.md).

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
| `?` | `?` | Print the current settings, state and rejected-frame count |

## Tuning the PID

The default gains are only a starting point. Every enclosure heats differently,
so they need tuning on yours. Start a long run (e.g. `M600`, then `G`) and use
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
| `RELAY_PINS` | D2–D7 | Relay input pins for heaters 1–6 |
| `RELAY_ACTIVE_LOW` | true | `true` for relay modules that switch on with a LOW input |
| `WINDOW_MS` | 10000 | Cycle time for the one switching relay (can be ~2000 with SSRs) |
| `MIN_PULSE_MS` | 1000 | Shortest relay on/off pulse allowed |
| `NEX_BAUD` | 9600 | Nextion serial speed (must match the screen) |
| `MAX_SAFE_F` | 560.0 | Over-temperature cutoff (°F) |
| `MAX_BAD_READS` | 4 | Consecutive sensor faults before shutdown |
| `RUNAWAY_TIME_MS` / `RUNAWAY_MIN_RISE_F` | 10 min / 15 °F | Thermal runaway detection |
| `OVERSHOOT_LIMIT_F` | 20.0 | Fault if the temperature goes this far above the setpoint during a run |
| `PIN_ESTOP` | A0 | E-stop input (NC contact to GND) |
| `ESTOP_DEBOUNCE_MS` | 20 | E-stop must read open this long before tripping (filters noise) |
| `REQUIRE_SCREEN_LINK` | true | Shut off if the screen link is lost during a run; `false` only for USB bench testing |
| `SCREEN_TIMEOUT_MS` | 5000 | How long without a screen heartbeat before shutting off |

## Safety features

- **Off until started:** heaters stay off at power-up and after a reset until
  Start is pressed.
- **Stop:** turns all heaters off immediately.
- **Run complete:** heaters turn off automatically when the hold time ends.

These faults **latch every heater off** and show **FAULT** until the board is reset:

- **E-stop:** the E-stop button is pressed, or its wire is broken. This is
  checked on every pass of the program and reacts within about 20 ms.
- **Overshoot:** during a run, the temperature goes more than 20 °F above the
  setpoint (e.g. 520 °F with a 500 °F setpoint). If you lower the setpoint
  mid-run, this check waits until the enclosure has cooled to within 20 °F
  of the new setpoint before it applies.
- **Screen link lost:** during a run, no heartbeat from the screen for 5 seconds.
- **Over-temperature:** the temperature reaches `MAX_SAFE_F` (560 °F), whatever the setpoint.
- **Sensor fault:** the thermocouple is open or shorted for about 1 second.
- **Thermal runaway:** the heaters run near full power for 10 minutes without
  the temperature rising 15 °F. This usually means the thermocouple has come
  loose or out of the enclosure.

Two more protections:

- **Watchdog:** if the code hangs, the board resets within 2 seconds, which turns the heaters off.
- **Safe startup:** all relays are set to OFF before the pins are enabled at boot.

The screen never controls the heaters directly. If it disconnects mid-run, the
Arduino keeps controlling the temperature and finishes the run.

> ⚠️ **Mains voltage warning:** Never drive a heater directly from an Arduino
> pin. Use relays rated for each heater's voltage and current, fuse the
> circuit, and add an independent high-limit thermostat or thermal fuse so the
> heaters shut off even if the Arduino or a relay fails (relays can fail welded
> ON). Keep the thermocouple wiring away from mains wiring to avoid noisy readings.

## Relay choice

PID control switches one relay every few seconds. Mechanical relays are
typically rated for about 100,000 operations, so the switching relay will wear
out over time. **Solid-state relays (SSRs)** are recommended for all six heaters. At a
minimum, use them on the heater that does the switching once the enclosure is
holding temperature (e.g. at ~40 % output, that is heater 3). With SSRs, `WINDOW_MS` can be
lowered to about 2000 for even steadier control.

## Project structure

```
heating_elements/
├── README.md
├── NEXTION.md                  Nextion screen build guide
├── heater_controller/
│   └── heater_controller.ino   Arduino sketch
└── relay_test/
    └── relay_test.ino          Relay wiring test: all relays on/off every 1.5 s
```

To check relay wiring before using the main sketch, upload `relay_test.ino`
with the heaters unplugged. The relays should click on and off every 1.5
seconds, matching the ON/OFF messages in the Serial Monitor.

## Roadmap

- [x] Nextion touch screen for setpoint, hold time, start and stop
- [ ] Multi-step profiles (e.g. 300 °F for 30 min, then 500 °F for 60 min)
- [ ] Save settings and tuned PID values to EEPROM
- [ ] Rotate which heater does the switching to spread relay wear
