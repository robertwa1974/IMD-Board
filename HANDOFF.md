# Handoff: DIY HV Insulation Monitor (IMD) for EV conversions

Owner: Rob. Written 2026-11-25 from a claude.ai design session, for continuing in Claude Code.
Status (updated 2026-11-25, Claude Code session): first-pass schematic exists (`hw/`, generated
from `hw/design.py`, ERC clean); part choices and calculations in `DESIGN_NOTES.md`. No PCB yet.
The IR155 PWM decoder (ESP32-S3) is now a PlatformIO project and builds for the M5Dial; it has
not been run on hardware.

Companion project: the contactor economizer board (separate repo). Its next spin adds the
stacking header and interlock described in section 6. That interface is the main constraint
on this board.

---

## 1. Goal

A Bender-like insulation monitoring device (IMD) for DIY EV conversions:
- Measures insulation resistance HV+ to chassis (Rp) and HV- to chassis (Rn).
- Works with the ZombieVerter VCU and Rob's M5Dial display.
- Output-compatible with the Bender IR155 (MHS PWM + OKHS), so existing IR155 wiring,
  decoders and integrations work unchanged.
- Open-source friendly; stacks on top of the economizer board but must also work stand-alone.

Target first vehicle: son's truck, 96S lithium pack (~400 V nominal), Lexus GS450h inverter/motors.
Design for up to ~450 V (96S NMC, max 403 V). The 800 V option was dropped on 2026-11-26.

Thresholds (FMVSS 305 / ECE R100 / ISO 6469-3): fault < 100 ohm/V DC-only, warn < 500 ohm/V.
At 400 V: fault ~40 kohm, warn ~200 kohm.

## 2. Measurement method: switched-resistor bridge

- Sense dividers measure Vbus (HV+ to HV-) and Vn (chassis to HV-). Vp = Vbus - Vn.
- A known resistor R0 is switched alternately HV+ to chassis, then HV- to chassis.
- Skip the baseline (no-R0) phase: it is the slowest to settle and two phases are enough.

With x = 1/(Rn || Rdiv), g = 1/R0, A = Vp/Vn with R0 on HV+, C = Vp/Vn with R0 on HV-:

    x    = g * (1 + 1/C) / (1/A - 1/C)
    1/Rp = (x + g) / C
    1/Rn = x - 1/Rdiv          (Rdiv = chassis-to-HV- sense divider total)

Settling (dominant design problem):
- Chassis node time constant per phase: tau = (R_top || R_bot) * C_Y_total
  (R_top = everything HV+ to chassis, R_bot = everything chassis to HV-, including R0 when switched in).
- Example: 360 V, 200 nF total Y-cap, Rdiv 4 Mohm, R0 500 kohm, healthy Rp=Rn=20 Mohm
  -> tau ~ 85 ms per phase, ~1.2 s per P+N cycle settling to 0.1 % (6.9 tau).
- With a charger/DC-DC on the bus, Y-cap can be 1 uF+ -> multi-second cycles.
- Speed-up: 3 equally spaced samples, extrapolate V_inf = (V1*V3 - V2^2) / (V1 + V3 - 2*V2).
  Only after filtering; reject fits that disagree with the trend.
- MEASURE the real Y-cap first: LCR meter, HV disconnected, HV+ to chassis and HV- to chassis,
  inverter and DC-DC connected. Default assumption above is a guess.

R0 trade-off: smaller R0 = faster settling and more signal at the fault threshold, but the
isolation drops while R0 is in. It must never take the system below 500 ohm/V.
Starting point: R0 300-500 kohm for a 300-400 V pack. At the 100 ohm/V fault level the Vp/Vn ratio
only changes a few percent -> needs a good ADC (oversampling) and averaging.

Noise: inverter switching puts common-mode noise on both rails. Heavy filtering, averaging,
hold the last valid reading rather than tripping on a spike.

Reference design to start from: TI TIDUEZ8 (switched-bridge isolation monitor, solar inverters).

## 3. Architecture

HV side, referenced to HV- (single ADC domain):
- Sense dividers: Vbus and chassis-to-HV-. MOhm series strings of several resistors each, so one
  shorted resistor can't create a low-impedance path to chassis. HV-rated parts, creepage sized
  for pack voltage.
- R0 switches: PhotoMOS/SSR rated well above pack voltage (1 kV+), one for P side, one for N side.
- MCU + ADC on the HV side: STM32G0 family (same family as the economizer). The G031 is enough if
  outputs are IR155-style PWM only; use a G0B1 if CAN is added later.

Isolation barrier crossings only:
- Power: 12 V -> reverse protection + TVS -> small automotive buck to 5 V -> TI UCC12050
  (automotive variant UCC12051-Q1) -> 5 V isolated -> low-noise LDO to 3.3 V for MCU/ADC ref.
  UCC12050: 5 kVrms reinforced, 1697 VDC working, ~3.5 pF barrier, >1e12 ohm, 500 mW, input 4.5-5.5 V,
  SYNC pin (can sync to ADC sampling).
  Why low barrier C matters: the barrier sits between HV- and chassis = the measured node.
  Do NOT use generic "1 W 3 kV" SIP modules (hi-pot rating only, low/unspecified working voltage).
  Alternative: ADuM5401 (power + 4 data channels), but VIORM 560 V peak only.
- Signals: digital isolator for MHS/OKHS/IMD_OK outputs and CONT/INHIBIT inputs.
  If CAN is added: isolated CAN transceiver (e.g. TI ISO1042); check its working voltage rating.

Self-diagnostics (required, not optional):
- Two separate chassis connections (like Bender E/KE). Detect loss of either -> earth-connection fault.
- Self-test of R0 switches and front end. Report device error separately from insulation fault.
- A broken chassis wire must never read as perfect isolation.

HV tap: pack side of the contactors, after the pack fuse, with small HV fuses on the tap leads.
- Contactors open: monitors the pack only. Closed: monitors the whole HV system.
- Pack-side always has voltage, which the bridge method needs.
- Readings jump when contactors close (motor-side Y-caps and leakage join). Use CONT1/CONT2_CLOSED
  from the economizer to restart settling and label readings "pack only" vs "system".
- Optional: sense-only divider on the bus side for weld detection.
- DC fast charging: the charger runs its own IMD; this IMD must stop measuring (IMD_INHIBIT).

## 4. Outputs: Bender IR155-compatible

MHS (high-side PWM, ~Us - 2 V; receiver uses 2.2 kohm pull-down):
- 10 Hz Normal: duty 5-95 % encodes resistance, R = 90% * 1200k / (duty - 5%) - 1200k
  (5 % = open/no leakage, 95 % = short).
- 20 Hz Undervoltage (measurement still valid, same formula).
- 30 Hz Speed start: 5-10 % good, 90-95 % bad (fast estimate right after power-on).
- 40 Hz Device error (~50 % duty).
- 50 Hz Earth-connection (Kl.31 / E-KE) fault (~50 % duty). NOT an insulation fault.
OKHS (high-side): high = insulation OK and device healthy.

All of the above was written from IR155-32xx/42xx datasheets and memory. VERIFY against
Bender doc D00376 (IR155-3210) before freezing firmware.

Plus 3.3 V logic IMD_OK to the economizer (see section 6). Hold OKHS/IMD_OK LOW until the first
valid measurement after power-up.

## 5. Reference unit: Bender IR155-3210 (Rob owns one)

- CCS charging-station variant, 0-800 V, high-side outputs: MHS (PWM) and OKHS (status).
- Default fixed version: Ran 100 kohm, undervoltage detection inactive. If the article number ends
  CV4 it is the customizable version; check the label.
- Internal DC resistance >= 1.2 Mohm: it appears as leakage to any other IMD on the same bus.
- NEVER run the IR155 and the DIY IMD on the same HV system at the same time (they corrupt each
  other's measurement). For comparison testing, unplug the IR155's HV connector, don't just power it down.
- First successful measurement can take up to ~17.5 s after power-on (32xx datasheet).
- Use it: as accuracy/response-time reference with a decade box, and as interim protection via
  OKHS + the decoder below while the DIY board is developed.

## 6. Interface to the economizer board (approved for its next spin)

Economizer: STM32G031, 2-channel current-regulated coil driver, pure 12 V board, no HV.

Stacking connection:
- Economizer has a 2x6 2.54 mm socket (8.5 mm), bottom-left corner, away from coil current loops.
  IMD board uses an elevated male header to reach it.
- 15 mm M3 standoffs on the economizer's four corner holes, 69 x 59 mm grid. Use NYLON standoffs, or
  keep the IMD's mounting holes in its LV region with HV clearance (metal standoffs tie to economizer GND).
- IMD board needs a cut-out over economizer J1 (Micro-Fit 2x7) so the plug latch can be pressed.
- No HV parts on the IMD board's underside. Economizer J1 ~10 mm, C1 ~8 mm tall: check clearance.

Stacking header signals (pin numbers = economizer J4, economizer hw/design.py rev B):
| Pin | Economizer net | IMD board net  | Dir (from IMD view) | Level  | Notes |
|-----|----------------|----------------|---------------------|--------|-------|
| 1   | 12V_PROT       | 12V_IN         | in                  | 12 V   | Via 0.2 A PTC on economizer. IMD budget ~1 W max |
| 2,4,8 | GND          | GND            | -                   | -      | Economizer ground planes; logic and IMD supply return only |
| 3   | IMD_OK         | IMD_OK         | out                 | 3.3 V  | High = OK. Economizer has 100k pull-down: dead/missing IMD = not OK |
| 5   | CONT1_CLOSED   | CONT1_CLOSED   | in                  | 3.3 V  | Coil 1 energised at >= 60 % hold current (NOT proof of contact closure) |
| 6   | CONT2_CLOSED   | CONT2_CLOSED   | in                  | 3.3 V  | Same for channel 2. Channel-to-contactor mapping is documented, not fixed |
| 7   | IMD_INHIBIT    | IMD_INHIBIT    | in                  | 3.3 V  | High = stop measuring (e.g. DC fast charge). Economizer drives low for now |
| 9   | IMD_TX         | UART_TO_IMD    | **in**              | 3.3 V  | Economizer USART1 TX (PB6) -> IMD RX: commands (`imd send`). See DESIGN_NOTES "UART protocol" |
| 10  | IMD_RX         | UART_FROM_IMD  | **out**             | 3.3 V  | IMD TX -> economizer USART1 RX (PB7): `$IMD` status frames, information only |
| 11  | IMD_MHS        | IMD_MHS        | out                 | ~12 V  | IR155-style PWM, passed straight to economizer J1 pin 4 (harness) |
| 12  | IMD_OKHS       | IMD_OKHS       | out                 | ~12 V  | IR155-style status, passed straight to economizer J1 pin 11 |

UART naming: the economizer names pins 9/10 from its own side (IMD_TX = "economizer TX towards the
IMD"). An earlier version of this table had them reversed. The IMD board uses direction-neutral
names (UART_TO_IMD / UART_FROM_IMD) to avoid the ambiguity.
Economizer puts 1k series + ESD on the 3.3 V lines. The IMD side should protect its own outputs
(IMD_MHS/IMD_OKHS leave the vehicle harness).

Interlock policy (implemented in economizer firmware):
- An IMD fault blocks a contactor CLOSE; it never forces an OPEN under load. The VCU decides when to open.
- Setting `imd required|optional`, default optional (stand-alone builds aren't blocked).
- With required: IMD_OK must be high continuously for 2 s to count. Close request while not OK ->
  fault 0x40 "IMD inhibit", channel stays off, latched until the VCU drops the command (no auto-close;
  with the ZombieVerter, precharge then fails cleanly).
- IMD_OK drops while closed: warning only, channel stays closed.
- Power economizer + IMD from the same permanent 12 V feed as the ZombieVerter, so IMD_OK is valid
  before key-on.

## 7. Existing code: IR155 MHS decoder (ESP32-S3, Arduino/PlatformIO)

PlatformIO project at the repo root (`platformio.ini`, envs `imd`, `m5dial`, `native`):
- `lib/ir155_decoder/ir155_decoder.h/.cpp`: pure `decode(freq, duty)` + `dutyToKohm()` (host-testable),
  and an interrupt-driven `Decoder` class (both edges, 4-period averaging, 2-period state stability,
  250 ms no-edge timeout -> NO_SIGNAL with stuck level, 300 us glitch reject, IRAM-safe pin reads).
- `src/m5dial/main.cpp`: M5Dial example (Port B G1/G2, confirm for board revision), MHS + OKHS,
  own warn/fault thresholds (500k/100k), Serial output.
- `test/test_decode/`: Unity host tests of the decode logic.

    pio run -e m5dial        # builds (espressif32 6.13.0, board m5stack-stamps3)
    PATH=/c/msys64/mingw64/bin:$PATH pio test -e native     # MSYS2 gcc is already on Rob's PC

Status: ESP32 build OK, host tests pass (2026-11-26). Hardware NOT yet tested.

Input conditioning (per signal, MHS and OKHS):

    IR155 out --+-- 10k --+-- gate 2N7002/BSS138    drain -- 10k -- 3.3V, drain -> GPIO
                |          |                         source -> GND
              2.2k        10k (+ optional 12 V zener)
                |          |
               GND        GND

Inverting (code uses INPUT_INVERTED = true). Optional 1 nF gate-GND for noise.
Optocoupler (PC817, ~4.7k LED series) is an isolated alternative with the same inversion.

The same decoder reads the DIY IMD later, since outputs are IR155-compatible.

## 8. Open items / next steps

1. Measure the truck's (and Rabbit's) Y-cap per rail; update settling numbers and R0 choice.
2. Verify all IR155 encodings and the duty->R formula against Bender D00376.
3. Build the ESP32 decoder for the M5Dial; bench-test with the IR155 + decade box
   (1 M, 300 k, 100 k, 50 k from HV+ to chassis; confirm OKHS drops near 100 k; pull E or KE -> 50 Hz).
4. IMD schematic + PCB: FIRST PASS DONE (see `DESIGN_NOTES.md` for decisions: R0 480k, 4.8 MOhm sense
   strings, TPSI2140-Q1 switches instead of PhotoMOS, external ADS1220, E/KE dual divider for
   chassis-loss detection; 110 x 66 mm board overhanging the economizer by 34 mm; stand-alone
   harness plug J2 with 12 V-level contactor/inhibit inputs). Open: review,
   LCSC numbers for the generic parts, then order.
5. IMD firmware: FIRST PASS DONE (`src/imd/`, `lib/imd_core/`, see DESIGN_NOTES "Firmware").
   Host-tested against a simulated vehicle; not yet run on a Nucleo or the board.
6. Optional later: CAN output (STM32G0B1 + isolated CAN) and a ZombieVerter firmware module that
   treats an IMD fault as an opmode-blocking error; bus-side divider for weld detection.

## 9. Caveats

- DIY, uncertified. Fine for hobby use; not a substitute for a certified IMD in a product.
- Values marked "from memory" or "verify" have not been checked against primary datasheets.
