# Bench test: IMD firmware on a Nucleo-G031K8 (no HV)

Runs the real IMD firmware (`pio run -e imd`) on a Nucleo-G031K8 (same MCU as the board) against a
**scaled low-voltage model** of the measurement bridge. Nothing here is above 5 V.

What it proves before any IMD board exists: the ADS1220 driver, the P/N measurement cycle, the
bridge solve, speed start, OKHS trip and recovery, earth-connection (E/KE) detection, dead-switch
detection, inhibit, contactor scope, VBUS-transient handling and the 60 s stale warning, both
calibration steps, the MHS PWM encoding (cross-checked with the M5Dial decoder) and the `$IMD`
status frames.

What it does not prove: anything HV (isolation, creepage, the TPSI2140, the dividers' voltage
coefficient), the isolated supply, the IR155-style 12 V output stage.

## 1. The scaling trick

The firmware multiplies every ADC reading by the divider gain 4.818 M / 18 k = **267.67**. On the
bench the ADC reads the model's nodes directly, so:

- a **1.5 V AA cell** is a ~400 V pack (1.5 x 267.67 = 401.5 V);
- every **resistance** in the model is the real value **/ 100**, every **capacitance x 100** (same
  time constants). Readings and `CAL BRIDGE` values are in real-board units, i.e. bench ohms x 100.

| Real board | Bench model |
|---|---|
| Pack 400 V | AA cell, 1.5 V |
| R0 480 k (each side) | 4.7 k + the 4066 switch (~50-100 ohm) |
| E divider 4.818 M, KE divider 4.818 M | 47 k + 1.2 k each |
| Y-capacitance 200 nF | 22 uF (electrolytic is fine) |
| Leakage Rp / Rn | test resistor / 100 (table in section 5) |
| Rcal 1 M | 10 k, measured with the Fluke |
| TPSI2140 x 2 | 74HC4066 (two of its four switches) |

## 2. Parts

- Nucleo-G031K8, USB cable (power, programming and serial all over the ST-Link USB).
- ADS1220 breakout module, 3.3 V. Check its schematic: **CLK tied to DGND** (internal oscillator),
  REFP0/REFN0 not tied to anything that would load the inputs, no input dividers on AIN0-2.
- 74HC4066 (DIP-14), breadboard, jumper wires.
- 1 % resistors: 2 x 4.7 k, 2 x 47 k, 2 x 1.2 k, 2 x 10 k (VISO divider), 10 k (Rcal), 100 ohm,
  1 k, 3 x 10 k pull-downs, 3 x 1 k (LEDs), and the leakage set in section 5.
- 22 uF capacitor, 3 LEDs, AA cell + holder, a push button, the Fluke.
- Optional: oscilloscope or the M5Dial (section 6.9).

## 3. Wiring

### 3.1 Nucleo to the rest

| Nucleo pin | Port | Firmware use | Connect to |
|---|---|---|---|
| A2 | PA4 | ADC_CS | ADS1220 CS |
| A3 | PA5 | SPI SCK | ADS1220 SCLK |
| A6 | PA6 | SPI MISO | ADS1220 DOUT/DRDY |
| A7 | PA7 | SPI MOSI | ADS1220 DIN |
| D6 | PB0 | ADC_DRDY | ADS1220 DRDY |
| 3V3, GND | | | ADS1220 AVDD+DVDD, AVSS+DGND; 74HC4066 VDD (pin 14), VSS (pin 7) |
| D3 | PB1 | R0P_EN | 74HC4066 control 1 (pin 13) |
| D7 | PB2 | R0N_EN | 74HC4066 control 2 (pin 5) |
| A0 | PA0 | VISO_MON | middle of 10 k / 10 k from the Nucleo **5V** pin to GND (reads as VISO 5.0 V) |
| D9 | PA8 | MHS PWM | LED + 1 k to GND, and/or scope / M5Dial (section 6.9) |
| A5 | PA11 | OKHS | LED + 1 k to GND |
| A4 | PA12 | IMD_OK | LED + 1 k to GND |
| D12 | PB4 | CONT1 in | 10 k to GND; jumper to 3V3 = "contactor 1 closed" |
| D11 | PB5 | CONT2 in | 10 k to GND; jumper to 3V3 = "contactor 2 closed" |
| D1 | PB6 | INHIBIT in | 10 k to GND; jumper to 3V3 = inhibit |
| D0 | PB7 | heartbeat | (optional LED + 1 k) |

Serial is PA2/PA3 = the ST-Link USB port: `pio device monitor -b 115200`.
Nucleo-32 boards can have A4/A5 bridged to D4/D5 by solder bridges; check the Nucleo-G031K8 user
manual (UM2591) and leave D4/D5 unconnected.

### 3.2 The bridge model

```
   AA (+) ── HVP ───────────────┬──────────────┬───────────── AIN0  (VBUS)
   AA (-) ── GND (= HV-)        │              │
                               4.7k (R0P)     Rp (socket)
                                │              │
                    4066 sw1 (ctrl D3)         │
                                │              │
           AIN1 ── E ───────────┴──[JE]── CH ──┴──[JKE]── KE ── AIN2
                   │                      │                │
            47k+1.2k                  Rn (socket)      47k+1.2k
                   │                   22 uF               │
                  GND                     │               GND
                   │                     GND
            4066 sw2 (ctrl D7) from E, then 4.7k (R0N) to GND
```

- CH is the "chassis". JE and JKE are jumpers: the two chassis wires. Pulling one = a broken wire.
- R0P: HVP -> 4.7 k -> 4066 switch 1 -> E. R0N: E -> 4066 switch 2 -> 4.7 k -> GND.
- Rp from HVP to CH, Rn from CH to GND: leave empty for "no leakage".
- Optional for the transient test (6.8): 100 ohm in series between the cell and HVP, plus a push
  button that puts 1 k from HVP to GND (a ~9 % "acceleration" sag while pressed).

## 4. Build and flash

```
pio run -e imd -t upload
pio device monitor -b 115200
```

You should see the banner (`IMD fw start, ADS1220 ok`), the calibration line, a `$IMD,...` frame
every 0.5 s and a `# IMD ...` line every second. `LOG OFF` silences the `#` lines.

## 5. Expected readings

Firmware readings are real-board units (bench x 100). With the AA cell (~400 V equivalent):

| Bench Rp | Bench Rn | Expect Rp | Expect Rn | ohm/V at 400 V | OKHS |
|---|---|---|---|---|---|
| open | open | INF or very high | INF or very high | - | on |
| 200 k | 200 k | ~20 M | ~20 M | 50 k | on |
| 10 k | open | ~1 M | high | 2.5 k | on |
| 2 k | open | ~200 k | high | 500 | on (warn level) |
| 470 ohm | open | ~47 k | high | 117 | stays on if it was on; does **not** come on from a cold start (needs 125 ohm/V = 50 k): hysteresis |
| 330 ohm | open | ~33 k | high | 82 | **off** after 2 cycles |
| open | 330 ohm | high | ~33 k | 82 | **off** |

High values (> ~5 M real) are imprecise by design: the board's own 2.4 M dividers dominate there.

## 6. Tests

Tick each one; note the status line / frame when something looks wrong.

1. **No ADC.** Before wiring the ADS1220: banner says `NOT RESPONDING`; frames show `DEV`, MHS LED
   flickers at 40 Hz (looks steadily half-lit), OKHS and IMD_OK LEDs off.
2. **No pack.** ADS1220 wired, cell out: `DEV` (Vbus below 20 V), OKHS off.
3. **Healthy start.** Cell in, Rp/Rn open: the first cycle (~1.2 s) gives `SSTG` (30 Hz), OKHS still
   off; then `NORM`, OKHS on. Vbus ~400 (cell voltage x 267.67).
4. **Calibration** (DESIGN_NOTES "Calibration", bench units):
   - Measure the cell with the Fluke, e.g. 1.502 V -> 1.502 x 267.67 = 402.0.
   - Jumper E and KE to HVP (JE/JKE stay in): `CAL GAIN 402.0` -> `CAL OK`.
   - Remove that jumper. Rcal 10 k from HVP to CH, Rp/Rn empty. Measure Rcal, e.g. 9.98 k ->
     `CAL BRIDGE 998k` -> `CAL OK`, R0 ~470-480 k (the 4066's on-resistance is included: that is
     what calibration is for), Rdiv ~2.41 M.
   - `CAL` shows `steps=GB`. Remove Rcal.
5. **Leakage table.** Work down section 5. Rp/Rn should be within a few % (better at the low
   values). OKHS must drop for the 330 ohm rows and come back once the resistor is removed.
6. **Broken chassis wire.** Pull JE: within two cycles `EARTH`, MHS 50 Hz, OKHS off. Refit, it
   recovers. Same with JKE.
7. **Dead R0 switch.** Unplug the D3 wire and tie 4066 pin 13 to GND: after 3 unsolvable cycles
   `DEV`. Restore.
8. **Inhibit / scope / transients.**
   - D1 jumper to 3V3: both 4066 controls go low (meter on pins 13 and 5), last state held, flag 08
     in the frame. Remove: measuring resumes, no `DEG`.
   - D12 jumper to 3V3: the cycle restarts and the next result is labelled `scope=system` (frame
     flag 01). Remove.
   - Fit the 100 ohm series resistor. Press the button for a second now and then: `restarts=`
     counts up, no result is taken across a press. Keep pressing on and off for over 60 s:
     `state=DEGRADED` / frame state `DEG`, OKHS keeps its last value. Stop: it clears.
9. **MHS against the decoder.** D9 to the M5Dial input (G1) directly (3.3 V logic, common GND) with
   `INPUT_INVERTED = false` in `src/m5dial/main.cpp`. The M5Dial should print the same mode and
   about the same R as the `# IMD` line, for every row of section 5.
10. **Frames.** Copy a few `$IMD` lines from the monitor. Each should match the `# IMD` line, `seq`
    should go up by one, and the checksum must be right (the economizer's `imd` command will count
    `bad=` frames once stacked).

Not testable here: the R0 interlock (no command can request both switches; it guards against a code
bug), the watchdog (needs a deliberately hung build), anything on the HV side of the real board.

## 7. After the bench

Record Rp/Rn for each row, the CAL result and anything odd in an issue or in `DESIGN_NOTES.md`.
Once the truck's Y-capacitance is measured (bus-side test), set `maxPhaseMs` from it and repeat the
healthy and 330 ohm rows with that capacitance scaled onto the bench: C_bench = C_truck x 100.
