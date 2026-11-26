# IMD board: design notes (rev A, first pass: schematic, PCB, firmware)

Companion to `HANDOFF.md` (requirements) and `hw/design.py` (the netlist, single source of truth).
Everything here is for a 96S NMC pack (max 403 V), designed to the 450 V class. The 800 V option was
dropped (2026-11-26). Values marked **verify** were not checked against a primary source.

## Workflow

```
cd hw
python gen_lib.py      # imd.kicad_sym (UCC12050, TPSI2140-Q1), imd.pretty (DWQ0011A footprint)
python gen_sch.py      # imd.kicad_sch from design.py  (do not hand-edit the .kicad_sch)
kicad-cli sch export netlist -o imd.net imd.kicad_sch && python check_netlist.py
kicad-cli sch erc -o erc.rpt imd.kicad_sch
kicad-cli sch export pdf -o imd_schematic.pdf imd.kicad_sch
```
Same scripts and conventions as the economizer repo. `design.py` also refuses any non-barrier part
that touches both the LV and HV domains.

State at writing: 131 symbols, 91 nets, netlist matches design.py, ERC 0 violations.

## Domains

| Domain | Reference | Contents |
|---|---|---|
| LV | GND (chassis, via the 12 V system) | Stack header, stand-alone connector, buck, 3.3 V LDO, MHS/OKHS drivers, isolator side 2 |
| HV | GND_HV = HV- terminal | UCC12050 secondary, LP5907, STM32G031, ADS1220, sense dividers, TPSI2140 primaries |
| E/KE | chassis (separate wires) | Reaches the HV domain only through MOhm strings and the R0 switches |

Barrier parts: U3 UCC12050 (power), U4 ISO7763F, U5 ISO7721, and the two TPSI2140 (switch side vs
primary side). All have >= 8 mm creepage across the body.

## Changes from HANDOFF.md

- **R0 switches: TI TPSI2140-Q1 instead of PhotoMOS.** 1200 V standoff, 130 ohm, avalanche rated
  (2 mA / 5 s), AEC-Q100, made for exactly this job (TI markets it for insulation-resistance
  monitoring). Driven straight from an MCU pin (EN >= 2.1 V), VDD 4.5-20 V from VISO_HV, 9 mA when on,
  no LED resistor. I could not confirm a >= 1000 V PhotoMOS datasheet from here.
- **External ADC (ADS1220)** rather than STM32 oversampling: at the 100 ohm/V threshold the Vp/Vn
  ratio moves by a few percent, so offset/gain drift and reference noise dominate. ADS1220: 24-bit,
  internal 2.048 V reference, 4 single-ended inputs, 50/60 Hz rejection at 20 SPS.
- **Chassis-loss detection via two dividers (E and KE)**, see below.
- **UART pin naming** in the stacking table was reversed in the handoff; fixed (see HANDOFF.md section 6).

## Measurement front end

### Sense dividers
Each: 4 x 1.2 MOhm 1206 + 18 kOhm 0.1 % bottom + 1 k / 100 nF anti-alias.

- Full scale = 2.048 V x 4.818 M / 18 k = **548 V**. 400 V reads 1.49 V.
- 112 V per 1206 at 450 V; use parts rated >= 200 V each. One shorted part still leaves >= 3.6 MOhm.
- RC corner ~90 Hz (18 k || 4.8 M with 1 k + 100 nF). Inverter noise filtering is mostly digital.
- No input clamps: a failed bottom resistor still leaves 4.8 MOhm in series (<= 94 uA into the ADC's
  ESD structure), so a clamp's leakage error (uA x 18 k = several mV) would cost more than it saves.

| Channel | ADS1220 input | Measures |
|---|---|---|
| VBUS | AIN0 | HV+ to HV- (pack side) |
| VE   | AIN1 | E (chassis wire 1) to HV- |
| VKE  | AIN2 | KE (chassis wire 2) to HV- |
| -    | AIN3 | Unused, floating (the bus-side weld-detect divider was dropped to fit the board) |

### R0 bridge
- R0P: HV+ -> 4 x 120 k -> TPSI2140 (U8) -> E.   R0N: E -> TPSI2140 (U9) -> 4 x 120 k -> HV-.
  (TPSI2140 S1/S2 are interchangeable: back-to-back MOSFETs.)
- R0 = 480 k. With R0N on and both chassis dividers (4.818 M || 4.818 M = 2.409 M) the worst-case
  isolation the IMD itself imposes is 480 k || 2.409 M = 400 k = **889 ohm/V at 450 V** (> 500 ohm/V).
- R0 string worst case (chassis hard-shorted to one rail): 450^2 / 480 k = 0.42 W total,
  105 mW per 1206 (rated 250 mW).
- Settling (handoff example, Y = 200 nF, Rp = Rn = 20 M, Rdiv = 2.4 M): tau ~ 80 ms per phase.
  **Measure the real Y-capacitance before choosing firmware timing.**
- `1/Rn = x - 1/Rdiv` from the handoff with Rdiv = 2.409 M (both dividers).
- **Calibration:** strings and 18k bottoms are 1 % parts, so the three channels match only to ~1 %
  and R0 / Rdiv are only known to ~1 %. A one-time bench calibration fixes both (see "Calibration").
  The Vbus divider is across the pack and does not enter the chassis equations.

### Chassis-connection (E/KE) monitoring
E carries the R0 switches; KE only has its own divider. Both should read the same chassis voltage.
- E wire broken: during the R0P phase, E is pulled towards HV+ through R0 alone (VE ~ 0.9 x Vbus)
  while KE still reads the true chassis -> large mismatch.
- KE wire broken: VKE falls to 0 V while VE does not.
- Mismatch beyond tolerance -> MHS 50 Hz "earth connection fault", OKHS low.
  This meets the handoff requirement that a broken chassis wire never reads as perfect isolation.

### Device self-tests (firmware)
- R0 switch stuck open: VE does not move between phases -> device error (40 Hz).
- R0 switch stuck closed: both phases give the same ratio -> device error.
- VISO monitor (PA0, VISO/2), ADS1220 internal (AVDD-AVSS)/4 and reference monitors.
- HV- tap open: the HV domain floats, VBUS reads ~0 -> undervoltage / device error.

## Barrier and supplies

- **UCC12050** (U3; UCC12051-Q1 for automotive): SEL tied to VISO -> 5.0 V (datasheet: 4.7-5.3 V,
  above the TPSI2140's 4.4 V UVLO). EN pulled up; SYNC (pin 4) tied to GNDP for the internal
  oscillator; ~SYNC_OK (pin 5, open-drain diagnostic) deliberately unconnected. Datasheet: 1697 VDC
  working, ~3.5 pF barrier, no-load input 50 mA at 5 V.
  Input and output caps (C10, C12) are 1206 10 uF 50 V X5R: an 0805 10 uF loses roughly half its value
  at 5 V bias, and the datasheet asks for a real 10 uF on each side.
- **12 V input**: the stacking header (12V_STACK, via D1) and the stand-alone J2 (12V_J2, via D8) are
  diode-ORed into VLV, so connecting both can never back-feed the economizer's 12V_PROT through its PTC.
- **ISO7763F** (U4): 3 channels HV->LV (MHS, OKHS, IMD_OK) and 3 LV->HV (CONT1, CONT2, INHIBIT).
  F suffix = outputs default LOW when the other side is dead, so a dead HV side gives MHS = no
  signal, OKHS = low, IMD_OK = low. 1500 Vrms / 2121 VDC working.
- **ISO7721** (U5, non-F so the UART idles high): channel A LV->HV (RX), channel B HV->LV (TX).
- **LMR14206** buck 12 V -> 5 V, values from datasheet Fig. 10: 15 uH, 0.15 uF CBOOT, SS16 catch diode;
  feedback 5.6 k / 1 k (5.05 V, JLCPCB Basic parts instead of the datasheet's 5.62 k / 1.02 k). SHDN via 100 k from VLV (datasheet advice when it may exceed 42 V).
- LV 3.3 V: AP2112K. HV 3.3 V: LP5907 (low noise for MCU + ADC).

Power budget (estimate, 12 V input):

| Load | Estimate |
|---|---|
| UCC12050 no-load | 50 mA @ 5 V |
| HV side (MCU ~3 mA, ADC 0.4 mA, isolators ~6 mA, one TPSI 9-11 mA, LED 1 mA) | ~20 mA @ 5 V -> ~40 mA input at ~50 % |
| LV 3.3 V (isolator side 2, LED) | ~10 mA |
| Buck output total | ~100 mA @ 5 V = 0.5 W |
| From 12 V (85 % buck) + MHS/OKHS into 2.2 k loads | **~0.7 W, ~60 mA** |

Within the 1 W budget and the economizer's 0.2 A PTC.

## IR155-style outputs (Q1-Q4)
2N7002 -> BSS84 high-side switch -> 470 ohm 2512 -> output, SMAJ24CA to GND.
- High level into the IR155-standard 2.2 k load: 11.6 V x 2.2/2.67 = **~9.6 V** (IR155: ~Us - 2 V).
- Dead short to GND: 15.6^2 / 470 = 0.52 W in a 1 W 2512, indefinitely. No fuse or electronics needed.
- BSS84 Vgs = -VLV x 10/32: -3.6 V at 11.6 V, -12 V at the 38 V TVS clamp (abs max +/-20 V).
- Backfeed above VLV goes through the BSS84 body diode, limited by the 470 ohm.

## MCU pin map (STM32G031K8T6, HV side)

| Pin | Port | Net | Function |
|---|---|---|---|
| 7 | PA0 | VISO_MON | ADC_IN0, VISO/2 |
| 9/10 | PA2/PA3 | MCU_TX / MCU_RX | USART2 AF1, through ISO7721 |
| 11 | PA4 | ADC_CS | GPIO |
| 12-14 | PA5/6/7 | ADC_SCK/MISO/MOSI | SPI1 AF0 (ADS1220 SPI mode 1) |
| 15 | PB0 | ADC_DRDY | EXTI |
| 16/17 | PB1/PB2 | R0P_EN / R0N_EN | TPSI2140 EN, 100 k pull-downs |
| 18 | PA8 | MHS_HV | TIM1_CH1 AF2, 10-50 Hz PWM |
| 22/23 | PA11/PA12 | OKHS_HV / IMD_OK_HV | GPIO |
| 28-30 | PB4/5/6 | CONT1/CONT2/INHIBIT_HV | inputs |
| 31 | PB7 | LED_HB | heartbeat |
| 24/25, 6 | PA13/PA14, NRST | SWD | J5, **HV-referenced** |

## Connectors

| Ref | Part | Pins |
|---|---|---|
| J1 | 2x6 elevated male header | Matches economizer J4 (table in HANDOFF.md section 6) |
| J2 | Micro-Fit 3.0 1x4 | 12 V, GND, MHS, OKHS: for stand-alone use. 12 V is diode-ORed with J1, so both may be connected |
| J3 | Micro-Fit 3.0 1x3 (600 V rated) | 1 HV+ (pack side, after the pack fuse), 3 HV-. Pin 2 unloaded, no copper |
| J4 | Micro-Fit 3.0 1x2 | E, KE: two separate wires to chassis |
| J5 | 1x5 2.54 mm | SWD (HV side) |

HV tap leads get inline fuses in the harness (>= 1000 VDC rated, <= 1 A; **choose part**).

## PCB (rev A, first pass)

`python hw/gen_lib.py`, then with KiCad's python: `gen_pcb.py place` -> Freerouting -> `gen_pcb.py finish`,
then `python hw/fab.py`. Freerouting 2.4.1 comes with Rob's KiCad plugin:

```
java -jar %USERPROFILE%/Documents/KiCad/10.0/3rdparty/plugins/app_freerouting_kicad-plugin/jar/freerouting-2.4.1.jar      -de imd.dsn -dr imd.rules -do imd.ses -mp 150 --gui.enabled=false
```

State: DRC 0 errors, 0 unconnected (one intended warning: J3 footprint differs from the library
because its middle pin's copper is removed).

- **106 x 66 mm**: the economizer's 76 x 66 plus a 30 mm overhang on the right (HV side). Same four M3
  holes (69 x 59 grid) and stacking-header position as the economizer; two extra holes (H5, H6) support
  the overhang. **H2, H4, H5, H6 are in the HV domain: nylon standoffs and screws.**
- Notch over the economizer's J1 latch: x 25..51, y 0..8.
- **LV | barrier | HV**: LV x < 31, copper-free gap x 31..39 (keep-out area), barrier parts centred on
  x = 35 with milled 1.5 mm slots under them. All barrier parts have >= 8 mm creepage.
- HV zone A (x 39..59): LDO, MCU, ADC, sense low ends. Zone B: two TPSI2140, the five HV strings as
  horizontal rows (HV end on the right), HV connectors on the right edge.
- **J3 (HV+/HV-) and J4 (E/KE) on the right edge**, facing out; J3's unused middle position has no copper,
  so HV+ and HV- are 6 mm apart.
- HV netclass: 2.5 mm to everything (IEC 60664-1, PD2, material group III, 500 V row - **verify**).
  `hw/imd.kicad_dru` (generated from `STRINGS` in `gen_pcb.py`) relaxes this to 0.5 mm **only between
  neighbouring nodes** of a string (<= 112 V apart: 20 explicit pairs) and to 1.0 mm between E and KE
  (same potential except during a wire-break fault). Every other pair keeps 2.5 mm, so DRC checks the
  row ends that carry the full pack voltage: e.g. R0P_SW, which floats at HV+ whenever U8 is off,
  against the VE row (4.25 mm), and the R0N string's HV- end against the KE row (3.95 mm).
- Both copper layers: KiCad applies netclass clearance and the custom rules on every copper layer,
  zone fills included. `hw/check_hv_clearance.py` measures it independently: for every HV through-hole
  pad (J3, J4) and every HV via it prints the smallest gap on F.Cu and B.Cu to any other net's copper
  (the bottom GND_HV pour, tracks, vias, pads). There are no HV-net vias: all HV tracks are top-layer.
- **HV pin margin**: a further rule keeps all non-HV copper (pour, tracks, vias, pads) 3.5 mm from
  the HV through-hole pins (J3 HV+, J4 E/KE) - 1 mm over the netclass minimum, for the hand-soldered
  joints and flux residue on the side facing the economizer. `gen_pcb.py finish` removes any router
  GND_HV track inside that margin (the bottom pour connects both ends anyway).
  Measured (2026-11-26): bottom layer exactly 3.50 mm from the GND_HV pour at all three pins; top
  layer 4.5-6.9 mm; E-KE 1.50 mm (rule 1.0 mm); no HV-net vias.
- HV tracks are hand-placed and locked in `gen_pcb.py` (`hv_routes`); the autorouter sees them as
  obstacles. The low end of each string gets a locked escape stub + via.
- Pours: GND (LV) both layers in the LV region; GND_HV on the bottom across the whole HV domain and on
  the top in zone A plus a band below the strings. No pour between the HV strings on top.
- The bottom-layer GND_HV pour sits at HV- potential, facing the economizer 15 mm below (its tallest
  parts, J1 10 mm and C1 8 mm, leave >= 5 mm of air). Solder-masked; fine for 450 V, but do not mount
  the stack on metal standoffs on the right-hand holes.
- Mount in the HV contactor box; conformal-coat the HV side after testing (keep J3/J4/J5 clear).

Before ordering:
1. Every assembled part has an LCSC number (table `LCSC_GENERIC` in `design.py`, checked 2026-11-26).
   Hand-soldered: J1 (elevated 2x6 male header, length to suit 15 mm standoffs + the 8.5 mm socket),
   J5 (1x5 header), J4 (Molex 43650-0200: not at LCSC, buy from Digi-Key/Mouser). J2/J3 are at LCSC
   but through-hole, so they are hand-soldered too (or add them to JLCPCB's THT assembly).
2. JLCPCB rotation check on the CPL preview (SOIC/TSSOP/TSOT orientations).
3. Look at the 3D renders in `hw/fab/` and the routing in KiCad once yourself. Known compromise: one
   low-voltage divider trace (VKE_DIV) runs on top under the body of U9, 2.6 mm from its HV pad.
4. Check stock on the day: UCC12050 (C504571) had only ~40-120 pieces.

## Firmware (first pass)

`lib/imd_core/` holds all the logic and has no hardware dependencies; `src/imd/` is the thin STM32 layer.

| File | Job |
|---|---|
| `imd_bridge` | Solve Rp/Rn from the settled chassis fraction f = Vn/Vbus of each phase; forward model; 3-point extrapolation |
| `imd_mhs` | IR155-style MHS encoding: duty = 5 % + 108000 / (R[k] + 1200) |
| `imd_monitor` | State machine: phase P / N, settle-or-timeout per phase, solve, fault priority, OKHS hysteresis |
| `src/imd/ads1220` | ADS1220 driver: single-shot, 90 SPS, PGA bypassed, internal 2.048 V ref (SBAS501D) |
| `src/imd/main.cpp` | Sample sets VBUS, VE, VKE, VBUS (~45 ms), pins, TIM1 PWM on PA8, R0 interlock, watchdog, calibration commands, 1 Hz status line |

Behaviour:
- Tracks f = Vn/Vbus rather than raw volts, so pack-voltage changes under load do not look like settling.
- A phase ends when it is settled (3-window exponential fit says < 0.0005 to go, or the windows are flat)
  or at `maxPhaseMs` (4 s default; raise it once the Y-cap is measured). The final value is the
  extrapolated one when the tail is a clean exponential.
- First cycle after power-up: 600 ms phases -> speed-start 30 Hz good/bad. OKHS / IMD_OK stay low
  until the first full measurement.
- Priority: earth fault (E/KE differ by > max(10 V, 5 % Vbus) at the end of phase P, 2 cycles) >
  device error (Vbus < 20 V, VISO outside 4.5-5.6 V, ADC timeout, or 3 unsolvable cycles, e.g. a dead
  R0 switch) > undervoltage (off by default) > normal.
- OKHS drops when min(Rp, Rn) < 100 ohm/V x Vbus for 2 cycles; returns above 125 ohm/V.
- INHIBIT: both switches open, outputs hold the last state. Contactor change: cycle restarts,
  result labelled "pack" or "system".
- **Pack-voltage transients** (acceleration, regen). Two effects skew the Vn/Vbus ratio: the chassis
  node lags a VBUS change through the Y-capacitance (it takes part of the step at once, then settles
  with the same ~80 ms time constant the measurement uses), and the ADC reads VBUS and VE ~11 ms
  apart. A simultaneous-sampling ADC would only remove the second, so this is handled in firmware:
  - each sample set reads VBUS before and after VE/VKE; the mean is used, the difference is checked;
  - a phase restarts if VBUS moved more than 0.5 % within it (or within one sample set), keeping the
    R0 switch as it is. Nothing measured across a transient is used.
  - After 5 restarts in a row (driving with continuous ripple), the phase is allowed to finish with a
    2 % limit and twice the averaging window. That cycle is flagged `relaxed=1` in the status line.
- **Held-reading age**: if no valid measurement completes for 60 s while HV is live and not
  inhibited, the status line reports `state=DEGRADED` with the age. This is a warning, not a fault:
  MHS / OKHS keep showing the last valid result. It clears with the next valid measurement.
  (Hard device faults - no HV, bad VISO - are reported as device errors instead.)
- **R0 interlock**: the output function refuses to turn on both R0 switches; if it is ever asked to,
  both stay off and the board latches a device error (MHS 40 Hz, OKHS low) until reset. The state
  machine never requests it; this guards against a future bug.
- Independent watchdog (1 s): a hung loop resets the MCU, which drives every output low.
- Flash use ~50 kB, capped at 62 kB in `platformio.ini` (the last 2 kB page holds the calibration).
  Integer-only printf and a hand-written number parser: newlib's float printf / strtof cost 10-20 kB.

Host tests (`pio test -e native`, 32 tests): bridge solve over a 6 x 6 grid of Rp/Rn, extrapolation, MHS
encode -> IR155 decode round trip, and the state machine against a simulated vehicle (Y-cap,
leakage, broken E or KE wire, dead switch, no HV, inhibit, contactor change, 1 uF Y-cap, +/-0.5 V
noise, pack-voltage swings with the Y-cap step response: +/-3 % holds the reading and goes stale
after 60 s, +/-0.6 % finishes in relaxed mode within 10 %), and both calibration steps (recovery of
mis-built R0, Rdiv and channel gains; rejection of wrong wiring, a leaky bench chassis, bad Rcal and
no HV). Checked that the tests catch a disabled earth check, a disabled OKHS trip and a disabled
transient check.

Not done yet: ADS1220 AVDD/reference self-monitor, UART protocol to the economizer, MHS/OKHS
behaviour verified against Bender D00376, bench run on a Nucleo-G031K8.

## Calibration

Once per board, on the bench, before it goes in the truck. Code: `lib/imd_core/imd_cal.*` (maths,
host-tested) and the `CAL` commands in `src/imd/main.cpp`. Stored in the last flash page with a CRC;
a board without a valid calibration runs on nominal values and says `cal=--` in its status line.

What it corrects:
- **GAIN**: each ADC channel's gain (string + 18k + ADS1220), from one known voltage.
- **BRIDGE**: the board's actual R0 and E||KE divider resistance (Rdiv), from one known resistor.
  With Rp = Rcal and Rn open: `Gdiv = Gp (1 + c) / (c (1 + a))`, `G0 = a Gdiv - Gp`.
In the simulation (R0 +2.5 %, dividers -2.5 %, channel gains up to 1.2 % off), a 30 kOhm fault
reads 2.4 % high uncalibrated and exactly after calibration. Real-board accuracy is set by the
meter readings you type in.

**Calibrate near operating voltage.** Thick-film resistors have a voltage coefficient: a 1.2 MOhm
1206 at ~100 V per part reads slightly lower than at 25 V (typically tens of ppm/V, part-dependent).
The 4.8 MOhm strings see the full pack voltage, the 18k bottoms almost none, so the divider ratios
shift with voltage. Calibrate with the bench supply close to the pack voltage (350-400 V for this
truck), or, if the bench supply cannot reach that, calibrate lower and then verify in the truck at
pack voltage against the IR155 (same decade-box resistances, one IMD connected at a time). Also
remember the 100 ppm/K tempco: expect around 1 % of drift over the full temperature range, which is
well inside what a 100 ohm/V trip with 25 % hysteresis needs.

Equipment:
- Current-limited DC supply, ideally close to pack voltage (350-400 V; a few mA is plenty), output
  floating.
- The Fluke for the supply voltage and the Rcal value.
- Rcal: about 1 MOhm, rated >= 1 kV (e.g. Vishay VR37 1M), value measured with the Fluke.
- USB-serial adapter (3.3 V) on the stacking header pins 9 (TX into the IMD), 10 (RX), GND,
  or the economizer once it forwards the UART. 115200 baud, one command per line.
- 12 V to the board (stacking header or J2).

**HV safety:** the supply is lethal-current-capable at 400 V even when limited; work with one hand,
connect with the supply off, and discharge (both R0 switches are open between steps; the 4.8 MOhm
dividers bleed the board, not the supply's own capacitors).

Steps:
1. **GAIN.** HV supply on J3 (HV+ pin 1, HV- pin 3). Jumper J4 E **and** KE to HV+. Nothing else.
   Turn on, read HV+ to HV- with the Fluke at J3, then send `CAL GAIN 380.3` (your reading).
   About 4 s. It refuses if any channel is more than 10 % off (typing error, E/KE not jumpered).
2. **BRIDGE.** Supply off, remove the jumper. Join E and KE together and connect Rcal from HV+ to that
   node. E/KE must not touch anything else, in particular not the supply's earth or HV-. Supply on,
   send `CAL BRIDGE 1.002M` (your measured Rcal). Takes 10..30 s (one speed-start cycle, then four
   settled P/N cycles). It refuses if R0 or Rdiv come out more than 10 % off nominal (leakage on the
   bench, wrong value typed).
3. `CAL` shows the result (`steps=GB`, gain corrections in ppm, R0 and Rdiv in ohms).
   Check: put a second known resistor (e.g. 200k) from HV+ to E/KE: the status line's `Rp=` should
   read it within ~1 %, and `Rn=` should read open (`inf`) or very high.

`CAL ABORT` stops a running step; `CAL CLEAR` goes back to nominal values. During a step the board is
not monitoring: MHS shows device error (40 Hz), OKHS / IMD_OK stay low, INHIBIT and the contactor
inputs are ignored.

## Open / verify

1. IR155 encodings and duty->R formula vs Bender D00376 (unchanged from the handoff).
2. ~~ADS1220 unused REFP0/REFN0~~: now left floating, per SBAS501D section 9.1.5 (AVSS works but leaks more). AIN3 must float when the VBB divider is not fitted, which it does (only DNP parts on it).
3. IEC 60664-1 creepage numbers above: **verify**.
4. HV-tap fuse part.
5. SWD is HV-referenced: program only with J3 unplugged; later, a UART bootloader through the isolator.
6. Firmware: first pass done (see above). Next: bench run on a Nucleo with a resistor-divider HV simulator at safe voltage.
