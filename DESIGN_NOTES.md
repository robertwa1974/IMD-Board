# IMD board: design notes (schematic rev A, first pass)

Companion to `HANDOFF.md` (requirements) and `hw/design.py` (the netlist, single source of truth).
Everything here is for a 96S pack (~400 V nominal, design to 450 V), with the 800 V option kept open
where it is cheap. Values marked **verify** were not checked against a primary source.

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
Each: 5 x 1 MOhm 1206 + 18 kOhm 0.1 % bottom + 1 k / 100 nF anti-alias.

- Full scale = 2.048 V x 5.018 M / 18 k = **571 V**. 400 V reads 1.435 V.
- 90 V per 1206 at 450 V (160 V at 800 V); stay with parts rated >= 200 V each.
- RC corner ~85 Hz (18 k || 5 M with 1 k + 100 nF). Inverter noise filtering is mostly digital.
- No input clamps: a failed bottom resistor still leaves 5 MOhm in series (<= 90 uA into the ADC's
  ESD structure), so a clamp's leakage error (uA x 18 k = several mV) would cost more than it saves.
- 800 V option: 18 k -> 12 k (FS 856 V), or 7 parts per string.

| Channel | ADS1220 input | Measures |
|---|---|---|
| VBUS | AIN0 | HV+ to HV- (pack side) |
| VE   | AIN1 | E (chassis wire 1) to HV- |
| VKE  | AIN2 | KE (chassis wire 2) to HV- |
| VBB  | AIN3 | Bus side of the HV+ contactor (DNP; weld detect later) |

### R0 bridge
- R0P: HV+ -> 5 x 100 k -> TPSI2140 (U8) -> E.   R0N: E -> TPSI2140 (U9) -> 5 x 100 k -> HV-.
- R0 = 500 k. With R0N on and both chassis dividers (5 M || 5 M = 2.5 M) the worst-case isolation the
  IMD itself imposes is 500 k || 2.5 M = 417 k = **926 ohm/V at 450 V** (> 500 ohm/V rule).
- R0 string worst case (chassis hard-shorted to one rail): 450^2 / 500 k = 0.41 W total,
  81 mW per 1206. At 800 V that becomes 256 mW per part: use 10 parts or 2512s for that option.
- Settling (handoff example, Y = 200 nF, Rp = Rn = 20 M, Rdiv = 2.5 M): tau ~ 80 ms per phase.
  **Measure the real Y-capacitance before choosing firmware timing.**
- With Rdiv = E || KE = 2.5 M: `1/Rn = x - 1/Rdiv` from the handoff, Rdiv = 2.5 M (both dividers).
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

- **UCC12050** (U3; UCC12051-Q1 for automotive): SEL tied to VISO -> 5.0 V. EN pulled up, SYNC to GNDP
  (internal oscillator; SYNC to the ADC clock is a later option). Datasheet: 1697 VDC working,
  ~3.5 pF barrier, no-load input 50 mA at 5 V.
- **ISO7763F** (U4): 3 channels HV->LV (MHS, OKHS, IMD_OK) and 3 LV->HV (CONT1, CONT2, INHIBIT).
  F suffix = outputs default LOW when the other side is dead, so a dead HV side gives MHS = no
  signal, OKHS = low, IMD_OK = low. 1500 Vrms / 2121 VDC working.
- **ISO7721** (U5, non-F so the UART idles high): channel A LV->HV (RX), channel B HV->LV (TX).
- **LMR14206** buck 12 V -> 5 V, values from datasheet Fig. 10: 15 uH, 0.15 uF CBOOT, 5.62 k / 1.02 k
  (4.98 V), SS16 catch diode. SHDN via 100 k from VLV (datasheet advice when it may exceed 42 V).
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
| J2 | Micro-Fit 3.0 1x4 | 12 V, GND, MHS, OKHS: stand-alone only (do not feed 12 V on both J1 and J2) |
| J3 | Micro-Fit 3.0 1x5 (600 V rated) | 1 HV+ (pack side), 3 bus side (optional), 5 HV-. Pins 2/4 unloaded |
| J4 | Micro-Fit 3.0 1x2 | E, KE: two separate wires to chassis |
| J5 | 1x5 2.54 mm | SWD (HV side) |

HV tap leads get inline fuses in the harness (>= 1000 VDC rated, <= 1 A; **choose part**).

## PCB rules to carry into layout

- HV domain <-> LV domain: **>= 8 mm creepage**, routed slot under U3/U4/U5/U8/U9 to match their
  body creepage. Nothing of the HV domain on the bottom side (stacked over the economizer).
- Inside the HV domain (HV+ / bus / E / KE vs GND_HV): IEC 60664-1, PD2, material group III,
  500 V -> 2.5 mm; 1000 V -> 5.0 mm (**verify against the table**). Use >= 5 mm on HV+ nets to keep
  the 800 V option.
- J3: delete the copper of positions 2 and 4 (custom footprint) so HV+ / bus / HV- are 6 mm apart.
- Resistor strings: lay them out in a straight line, with a slot between strings at different potentials.
- J1 footprint on the top layer at the same XY and rotation as economizer J4; the header is
  inserted from below. Mounting holes on the 69 x 59 mm grid; cut-out over economizer J1 latch.
- TPSI2140: use the HV land pattern (already in `imd.pretty`); SM pin is thermal only: copper
  pour, no net.
- Guard ring around the ADS1220 inputs at GND_HV (MOhm nodes are leakage-sensitive: keep the
  board clean, conformal coat after test).

## Open / verify

1. IR155 encodings and duty->R formula vs Bender D00376 (unchanged from the handoff).
2. ADS1220 unused REFP0/REFN0 tied to AVSS: **verify** against the ADS1220 datasheet.
3. IEC 60664-1 creepage numbers above: **verify**.
4. HV-tap fuse part.
5. SWD is HV-referenced: program only with J3 unplugged; later, a UART bootloader through the isolator.
6. Firmware (STM32G031 env) not started; state machine per HANDOFF.md section 8 item 5.
