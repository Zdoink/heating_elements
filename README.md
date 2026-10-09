# Arduino Heating Elements

Temperature-controlled heating enclosure built around an **Arduino Uno R3**.
A **Type K thermocouple** measures the enclosure temperature, and the Arduino
uses **PID control** to drive **6 heating elements** through relays, holding a
steady **500 °F**.

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

## Hardware

| Part | Notes |
|---|---|
| Arduino Uno R3 | Controller |
| MAX31855 **or** MAX6675 board | Thermocouple amplifier with SPI output |
| Type K thermocouple | Rated well above 500 °F; yellow = +, red = − |
| 6-channel relay module or 6 SSRs | Each rated for one heater's voltage and current |
| 6 heating elements | |
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

Thermocouple leads go into the amplifier board's screw terminal (yellow to +,
red to −). If the relay module has a JD-VCC jumper, a separate 5 V supply for
the relay coils is recommended because 6 coils draw a lot of current.

## Software setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software).
2. In **Library Manager**, install the library for your board:
   - MAX31855: **Adafruit MAX31855 library**
   - MAX6675: **MAX6675 library** by Adafruit
3. Open `heater_controller/heater_controller.ino`.
4. Set `USE_MAX31855` at the top of the sketch: `1` for a MAX31855 board, `0` for a MAX6675 board.
5. Select **Board: Arduino Uno** and the correct port, then upload.
6. Open the **Serial Monitor** or **Serial Plotter** at **9600 baud**:
   ```
   Temp_F:499.6 Setpoint_F:500.0 Output_pct:41.8
   ```

### First power-up check

Before connecting the heaters, upload the sketch with the thermocouple at room
temperature. All relays should click **ON** (the temperature is far below the
setpoint). If they are all **OFF**, or turn on only when the board is reset,
your relay module is active-HIGH: set `RELAY_ACTIVE_LOW = false`.

## Serial commands

Type into the Serial Monitor and press Enter. Changes last until the board is
reset; copy the values you settle on into the sketch.

| Command | Example | Effect |
|---|---|---|
| `S<temp>` | `S500` | Set the setpoint in °F |
| `P<value>` | `P3.0` | Set Kp |
| `I<value>` | `I0.02` | Set Ki |
| `D<value>` | `D10` | Set Kd |
| `?` | `?` | Print the current settings |

## Tuning the PID

The default gains are only a starting point. Every enclosure heats differently,
so they need tuning on yours. Use the Serial Plotter to watch the temperature.

1. Start with `I0` and `D0`. Raise `P` until the temperature settles close to
   500 °F with only a small, slow oscillation.
2. Raise `I` slowly (e.g. 0.005 → 0.01 → 0.02) until the temperature settles
   **on** 500 °F instead of slightly below it. If it starts oscillating, lower `I`.
3. If it overshoots after the warm-up or after the door is opened, add a little
   `D` (start around 5–20).
4. Put the final values into `Kp`, `Ki` and `Kd` in the sketch.

## Configuration

| Constant | Default | Meaning |
|---|---|---|
| `USE_MAX31855` | 1 | 1 = MAX31855 board, 0 = MAX6675 board |
| `setpointF` | 500.0 | Target temperature (°F) |
| `Kp`, `Ki`, `Kd` | 3.0, 0.02, 0.0 | PID gains (output in % of total power) |
| `INTEGRAL_BAND_F` | 40.0 | Integral only builds within ± this many °F of the setpoint, to prevent warm-up overshoot |
| `RELAY_PINS` | D2–D7 | Relay input pins for heaters 1–6 |
| `RELAY_ACTIVE_LOW` | true | `true` for relay modules that switch on with a LOW input |
| `WINDOW_MS` | 10000 | Cycle time for the one switching relay (can be ~2000 with SSRs) |
| `MIN_PULSE_MS` | 1000 | Shortest relay on/off pulse allowed |
| `MAX_SAFE_F` | 560.0 | Over-temperature cutoff (°F) |
| `MAX_BAD_READS` | 4 | Consecutive sensor faults before shutdown |
| `RUNAWAY_TIME_MS` / `RUNAWAY_MIN_RISE_F` | 10 min / 15 °F | Thermal runaway detection |

## Safety features

All faults **latch every heater off** until the board is reset:

- **Over-temperature:** the temperature reaches `MAX_SAFE_F`.
- **Sensor fault:** the thermocouple is open or shorted for about 1 second.
- **Thermal runaway:** the heaters run near full power for 10 minutes without
  the temperature rising 15 °F. This usually means the thermocouple has come
  loose or out of the enclosure.
- **Watchdog:** if the code hangs, the board resets within 2 seconds, which turns the heaters off.
- **Safe startup:** all relays are set to OFF before the pins are enabled at boot.

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
arduino_heating_elements/
├── README.md
└── heater_controller/
    └── heater_controller.ino
```

## Roadmap

- [ ] Display (LCD/OLED) for the current temperature and setpoint
- [ ] Buttons or a knob to change the setpoint without a computer
- [ ] Save tuned PID values to EEPROM
- [ ] Rotate which heater does the switching to spread relay wear
