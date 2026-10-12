# Nextion Screen Setup

How to build the touch screen for the heater controller in the free
[Nextion Editor](https://nextion.tech/nextion-editor/). The screen lets you set
the temperature and hold time, start and stop a run, and see the live status.

## How the screen and Arduino talk

The Arduino is always in charge. The screen only **requests** changes and then
displays what the Arduino reports back.

| Direction | What is sent | How errors are handled |
|---|---|---|
| Screen → Arduino | `<S500,29500>` setpoint, `<M90,29910>` minutes, `<GO>` start, `<X>` stop | Each frame starts with `<` and ends with `>`. Numbers carry a check value (value + check = 30000). The Arduino drops any frame that is damaged or doesn't add up. |
| Arduino → screen | Temperature, status, time left and power every second, plus `aSet`, `aMin`, `aState` | The screen resends a request every 0.5 s until the Arduino reports the new value back, so a lost or rejected frame is simply sent again. |
| Link check (screen side) | Arduino sets `link` to 0 every second; the screen counts it up | If nothing arrives for about 3 seconds, the screen shows **NO LINK**. |
| Link check (Arduino side) | Screen sends a heartbeat `<H>` every 0.5 s | If nothing valid arrives from the screen for 5 seconds **during a run**, the Arduino latches all heaters off (FAULT: screen link lost). |

`aState` values: `0` IDLE, `1` HEATING, `2` HOLDING, `3` DONE, `4` FAULT.

## 1. Create the project

1. **File → New**, pick your screen's exact model (printed on the back, e.g. NX4832T035) and orientation.
2. Add a font: **Tools → Font Generator**, height 24 or larger, then **Generate font** and add it to the project.
3. Baud rate stays at the default **9600** (matches the Arduino).

## 2. Add the components to page0

Names must match **exactly** (they are case-sensitive), because the Arduino
writes to them by name.

### Display boxes (Text)

| Name | Shows | Set `txt_maxl` to |
|---|---|---|
| `tTemp` | Current temperature, e.g. `499.6` | 10 |
| `tStatus` | IDLE / HEATING / HOLDING / DONE / FAULT / NO LINK | 10 |
| `tLeft` | Hold time left, e.g. `1:23:45` | 10 |
| `tOut` | Heater power, e.g. `42%` | 6 |
| `tMsg` | Fault reason, e.g. `E-stop pressed or E-stop wire broken` (blank when there's no fault). Make it wide, or use a small font. | 60 |

Add plain labels next to them ("Temp °F", "Status", "Time left", "Power") as you like.

### Settings (Number)

| Name | Meaning | Initial `val` |
|---|---|---|
| `nTemp` | Target temperature °F | 500 |
| `nMin` | Hold time in minutes | 60 |

### Buttons (Button)

`bTempUp`, `bTempDn`, `bMinUp`, `bMinDn`, `bStart`, `bStop`

### Variables (Variable — not visible on screen)

| Name | `sta` | Initial value |
|---|---|---|
| `aSet` | Number | 0 |
| `aMin` | Number | 0 |
| `aState` | Number | 0 |
| `link` | Number | 0 |
| `dirtyT` | Number | 0 |
| `dirtyM` | Number | 0 |
| `wantRun` | Number | 0 |
| `wantStop` | Number | 0 |
| `va0` | String | `txt_maxl` 10 |
| `va1` | String | `txt_maxl` 10 |

### Timer

Add a **Timer** named `tm0` with `tim` = **500** and `en` = **1**.

## 3. Add the code

Click a component, then paste the code into the event tab shown. In Nextion
code, `{` and `}` must be on their own lines exactly as shown.

### page0 → Preinitialize Event

```
bkcmd=0
```

### bTempUp → Touch Release Event

```
nTemp.val+=5
if(nTemp.val>535)
{
  nTemp.val=535
}
dirtyT.val=1
```

### bTempDn → Touch Release Event

```
nTemp.val-=5
if(nTemp.val<100)
{
  nTemp.val=100
}
dirtyT.val=1
```

### bMinUp → Touch Release Event

```
nMin.val+=5
if(nMin.val>1440)
{
  nMin.val=1440
}
dirtyM.val=1
```

### bMinDn → Touch Release Event

```
nMin.val-=5
if(nMin.val<1)
{
  nMin.val=1
}
dirtyM.val=1
```

### bStart → Touch Release Event

```
wantStop.val=0
wantRun.val=1
prints "<GO>",0
```

### bStop → Touch Release Event

```
wantRun.val=0
wantStop.val=4
prints "<X>",0
```

### tm0 → Timer Event

This runs every 0.5 s. It resends anything the Arduino hasn't confirmed yet,
and shows the Arduino's values whenever you aren't editing them.

```
// ---- heartbeat: tells the Arduino the screen is still connected ----
prints "<H>",0
// ---- link check: the Arduino resets link to 0 every second ----
link.val+=1
if(link.val>6)
{
  tStatus.txt="NO LINK"
}
// ---- target temperature ----
if(dirtyT.val==1)
{
  if(aSet.val==nTemp.val)
  {
    dirtyT.val=0
  }else
  {
    covx nTemp.val,va0.txt,0,0
    sys0=30000-nTemp.val
    covx sys0,va1.txt,0,0
    prints "<S",0
    prints va0.txt,0
    prints ",",0
    prints va1.txt,0
    prints ">",0
  }
}else
{
  if(aSet.val>0)
  {
    nTemp.val=aSet.val
  }
}
// ---- hold time ----
if(dirtyM.val==1)
{
  if(aMin.val==nMin.val)
  {
    dirtyM.val=0
  }else
  {
    covx nMin.val,va0.txt,0,0
    sys0=30000-nMin.val
    covx sys0,va1.txt,0,0
    prints "<M",0
    prints va0.txt,0
    prints ",",0
    prints va1.txt,0
    prints ">",0
  }
}else
{
  if(aMin.val>0)
  {
    nMin.val=aMin.val
  }
}
// ---- start: resend until the Arduino reports HEATING/HOLDING (or FAULT) ----
if(wantRun.val==1)
{
  if(aState.val==0||aState.val==3)
  {
    prints "<GO>",0
  }else
  {
    wantRun.val=0
  }
}
// ---- stop: send at least 4 times, and keep sending while still running ----
if(wantStop.val>0)
{
  prints "<X>",0
  if(aState.val==1||aState.val==2)
  {
    wantStop.val=4
  }else
  {
    wantStop.val-=1
  }
}
```

Sending Start or Stop more than once is safe: the Arduino ignores Start while a
run is already going (the timer is not restarted), and Stop when nothing is running.

## 4. Load it onto the screen

1. Click **Compile** and fix any errors shown at the bottom.
2. **File → TFT file output**, save the `.tft` file.
3. Copy the `.tft` file alone onto a **FAT32 microSD card** (32 GB or smaller).
4. With the screen powered off, insert the card, power it on and wait for
   "Update successed". Power off, remove the card, power on again.

## 5. Test

1. Wire the screen to the Arduino (see the README wiring table) and power both up
   with the **heaters unplugged**.
2. Within a second, `tTemp` should show the room temperature and `tStatus` **IDLE**.
3. Press **+** on the temperature. The number should stay where you set it
   (the Arduino confirmed it). The Arduino Serial Monitor shows no
   "Rejected screen frame" lines.
4. Press **Start**: status changes to **HEATING** and the relays click.
5. Press **Stop**: status returns to **IDLE** and all relays turn off.
6. **Link loss during a run:** press Start, then unplug the screen's blue (TX)
   wire. Within 5 seconds the relays turn off, and the Serial Monitor shows
   `FAULT: screen link lost`. Reset the Arduino to clear it.
7. **Link loss, Arduino side:** unplug the Arduino's TX wire (D9) instead.
   The screen shows **NO LINK** within a few seconds.
8. **E-stop:** press Start, then press the E-stop. The relays turn off
   immediately, and `tMsg` shows the E-stop message. Release the E-stop and
   reset the Arduino to clear it.
