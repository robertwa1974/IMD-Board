"""IMD board: single source of truth for parts and connectivity.

gen_sch.py draws the KiCad schematic from this. Each part: ref, lib_id, value, footprint,
pin->net map, plus BOM fields and a block name (the schematic groups parts by block).
Design reasoning and calculations: DESIGN_NOTES.md.

Three potential domains:
  LV   : GND (= vehicle chassis via the 12 V system), 12 V, 5V_LV, 3V3_LV.
  HV   : GND_HV (= HV- pack terminal), VISO_HV (5 V isolated), 3V3_HV. MCU + ADC live here.
  E/KE : chassis connections of the measurement bridge. Same potential as LV GND in the
         vehicle, but they reach the HV domain only through MOhm strings and the R0 switches.
Only the UCC12050, ISO7763, ISO7721 and TPSI2140 bodies cross the HV/LV barrier.
"""
from collections import OrderedDict

FP_R0603 = "Resistor_SMD:R_0603_1608Metric"
FP_R1206 = "Resistor_SMD:R_1206_3216Metric"
FP_R2512 = "Resistor_SMD:R_2512_6332Metric"
FP_C0603 = "Capacitor_SMD:C_0603_1608Metric"
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_C1206 = "Capacitor_SMD:C_1206_3216Metric"
FP_SOT23 = "Package_TO_SOT_SMD:SOT-23"
FP_SOT235 = "Package_TO_SOT_SMD:SOT-23-5"
FP_SOT236 = "Package_TO_SOT_SMD:SOT-23-6"
FP_SMA = "Diode_SMD:D_SMA"
FP_LED = "LED_SMD:LED_0603_1608Metric"
FP_SOIC16W = "Package_SO:SOIC-16W_7.5x10.3mm_P1.27mm"

parts = []          # list of dicts, in schematic order
_counters = {}


def _next_ref(prefix):
    used = {p["ref"] for p in parts}   # skip refs given explicitly (e.g. D8)
    while True:
        _counters[prefix] = _counters.get(prefix, 0) + 1
        ref = f"{prefix}{_counters[prefix]}"
        if ref not in used:
            return ref


def part(prefix, lib_id, value, footprint, pins, block, mpn="", lcsc="", note="", ref=None, dnp=False):
    p = dict(ref=ref or _next_ref(prefix), lib_id=lib_id, value=value, footprint=footprint,
             pins={str(k): v for k, v in pins.items()}, block=block, mpn=mpn, lcsc=lcsc, note=note, dnp=dnp)
    parts.append(p)
    return p


def R(value, a, b, block, fp=FP_R0603, note="", mpn="", dnp=False):
    return part("R", "Device:R", value, fp, {1: a, 2: b}, block, note=note, mpn=mpn, dnp=dnp)


def C(value, a, b, block, fp=FP_C0603, note="", dnp=False):
    return part("C", "Device:C", value, fp, {1: a, 2: b}, block, note=note, dnp=dnp)


def hv_string(top, bottom, value, n, block, prefix, note, dnp=False):
    """n equal 1206 resistors in series: no single short makes a low-impedance path,
    and each part sees only 1/n of the voltage."""
    nodes = [top] + [f"{prefix}_{i}" for i in range(1, n)] + [bottom]
    for i in range(n):
        R(value, nodes[i], nodes[i + 1], block, FP_R1206, note=note if i == 0 else "", dnp=dnp,
          mpn="1206 thick film 1 %, 200 V rated (112 V per part at 450 V)")


# =========================================================================== LV side
# --------------------------------------------------------------------------- connectors
B = "Stack header"
# Pin numbers match the economizer's J4 socket (economizer hw/design.py). UART names are from
# the signal's point of view, so there is no TX/RX ambiguity:
#   pin 9  UART_TO_IMD   = economizer TX (PB6) -> IMD RX
#   pin 10 UART_FROM_IMD = IMD TX -> economizer RX (PB7)
part("J", "Connector_Generic:Conn_02x06_Odd_Even", "Stack header 2x6",
     "Connector_PinHeader_2.54mm:PinHeader_2x06_P2.54mm_Vertical",
     {1: "12V_STACK", 2: "GND", 3: "IMD_OK", 4: "GND", 5: "CONT1_CLOSED", 6: "CONT2_CLOSED",
      7: "IMD_INHIBIT", 8: "GND", 9: "UART_TO_IMD", 10: "UART_FROM_IMD", 11: "IMD_MHS", 12: "IMD_OKHS"},
     B, ref="J1", mpn="2x6 elevated male header, 2.54 mm (length to suit 15 mm standoffs + 8.5 mm socket)",
     note="Footprint on top side at the same XY/rotation as economizer J4; header inserted from below")

B = "Stand-alone connector"
# Everything a stand-alone IMD needs on one harness plug (pins 1-6 outer row, 7-12 inner row).
# The 12 V-level inputs and the UART are conditioned in the "Stand-alone inputs" block (end of file).
part("J", "Connector_Generic:Conn_02x06_Top_Bottom", "Micro-Fit 2x6",
     "Connector_Molex:Molex_Micro-Fit_3.0_43045-1200_2x06_P3.00mm_Horizontal",
     {1: "12V_J2", 2: "GND", 3: "IMD_MHS", 4: "IMD_OKHS", 5: "GND", 6: None,
      7: "CONT1_J2", 8: "CONT2_J2", 9: "INHIBIT_J2", 10: "GND", 11: "UART_IN_J2", 12: "UART_OUT_J2"},
     B, ref="J2", mpn="Molex 43045-1200", lcsc="C277636",
     note="Stand-alone: 1 +12V, 2/5/10 GND, 3 MHS, 4 OKHS, 6 spare, 7 CONT1, 8 CONT2, 9 INHIBIT (12 V level), "
          "11 UART into IMD, 12 UART from IMD (3.3 V)")

# --------------------------------------------------------------------------- LV power
B = "LV power input"
part("D", "Diode:SS14", "SS14", FP_SMA, {1: "VLV", 2: "12V_STACK"}, B, mpn="SS14", lcsc="C2480",
     note="Stack input. With D8: diode-OR, so J2 can never back-feed the economizer 12V_PROT")
part("D", "Diode:SS14", "SS14", FP_SMA, {1: "VLV", 2: "12V_J2"}, B, mpn="SS14", lcsc="C2480", ref="D8",
     note="Stand-alone input (J2), reverse polarity; diode-OR with D1")
part("D", "Device:D_Zener", "SMAJ24A", FP_SMA, {1: "VLV", 2: "GND"}, B, mpn="SMAJ24A (unidirectional TVS)", lcsc="C3001960",
     note="Load-dump clamp for stand-alone use (economizer already clamps when stacked)")
C("10u 50V", "VLV", "GND", B, FP_C1206)
C("100n 50V", "VLV", "GND", B)
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "VLV"}, B, ref="#FLG01")
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "GND"}, B, ref="#FLG02")
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "12V_STACK"}, B, ref="#FLG03")
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "12V_J2"}, B, ref="#FLG06")

B = "LV 5 V buck"
part("U", "Regulator_Switching:LMR14206", "LMR14206XMK", "Package_TO_SOT_SMD:TSOT-23-6",
     {1: "BUCK_CB", 2: "GND", 3: "BUCK_FB", 4: "BUCK_SHDN", 5: "VLV", 6: "BUCK_SW"}, B,
     mpn="LMR14206XMKE/NOPB", lcsc="C2071127", note="4.5-42 V in, 1.25 MHz, 0.6 A. Values from datasheet Fig. 10 (5 V out)")
R("100k", "VLV", "BUCK_SHDN", B, note="Datasheet: 100k from supply when SHDN may see >42 V")
C("10n", "BUCK_SHDN", "GND", B, note="Soft start")
C("2.2u 50V", "VLV", "GND", B, FP_C1206, note="CIN, at VIN/GND pins")
C("150n", "BUCK_CB", "BUCK_SW", B, note="CBOOT >= 0.15 uF")
part("D", "Diode:SS16", "SS16", FP_SMA, {1: "BUCK_SW", 2: "GND"}, B, mpn="SS16", lcsc="C2481",
     note="Catch diode, 60 V 1 A (datasheet example)")
part("L", "Device:L", "15u", "Inductor_SMD:L_Bourns_SRN6045TA", {1: "BUCK_SW", 2: "5V_LV"}, B,
     mpn="Bourns SRN6045-150M", lcsc="C3224592", note="1.9 A rated (datasheet asks >= 1.1 A); same 6x6 land as SRN6045TA")
R("5.6k", "5V_LV", "BUCK_FB", B, note="Vout = 0.765 V x (1 + 5.6k/1k) = 5.05 V (both JLCPCB Basic)")
R("1k", "BUCK_FB", "GND", B)
C("22u 16V", "5V_LV", "GND", B, FP_C1206)
C("22u 16V", "5V_LV", "GND", B, FP_C1206)
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "5V_LV"}, B, ref="#FLG05")

B = "LV 3.3 V"
part("U", "Regulator_Linear:AP2112K-3.3", "AP2112K-3.3", FP_SOT235,
     {1: "5V_LV", 2: "GND", 3: "5V_LV", 4: None, 5: "3V3_LV"}, B, mpn="AP2112K-3.3TRG1", lcsc="C51118")
C("1u", "5V_LV", "GND", B)
C("1u", "3V3_LV", "GND", B)
R("1k", "3V3_LV", "LED_PWR", B)
part("D", "Device:LED", "green", FP_LED, {1: "GND", 2: "LED_PWR"}, B, note="LV power")

# --------------------------------------------------------------------------- isolation barrier
B = "Isolated power (barrier)"
part("U", "imd:UCC12050", "UCC12050DVER", FP_SOIC16W,
     {3: "5V_LV", 1: "ISO_EN", 4: "GND", 5: None, 6: "GND", 7: "GND", 8: "GND", 2: "GND",
      14: "VISO_HV", 13: "VISO_HV", 10: "GND_HV", 11: "GND_HV", 12: "GND_HV",
      9: "GND_HV", 15: "GND_HV", 16: "GND_HV"}, B, mpn="UCC12050DVER (UCC12051QDVERQ1 automotive)", lcsc="C504571",
     note="SEL=VISO -> 5.0 V. SYNC=GNDP (internal osc). NC 6-8 to GNDP, NC 10-12 to GNDS per datasheet")
R("10k", "5V_LV", "ISO_EN", B, note="EN high = on")
C("10u 50V", "5V_LV", "GND", B, FP_C1206, note="At VINP pin 3 / GNDP pin 2. 1206 50 V: keeps most of 10 uF at 5 V bias")
C("100n", "5V_LV", "GND", B)
C("10u 50V", "VISO_HV", "GND_HV", B, FP_C1206, note="At VISO pin 14 / GNDS pin 15. 1206 50 V: keeps most of 10 uF at 5 V bias")
C("100n", "VISO_HV", "GND_HV", B)
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "GND_HV"}, B, ref="#FLG04")

B = "Signal isolation (barrier)"
# ISO7763F: side 1 = HV (A,B,C in -> out on side 2; D,E,F in on side 2 -> out on side 1).
# F suffix: outputs default LOW when the input side is unpowered -> dead HV side reads "not OK".
part("U", "Isolator:ISO7763DW", "ISO7763FDWR", FP_SOIC16W,
     {1: "3V3_HV", 8: "GND_HV", 2: "MHS_HV", 3: "OKHS_HV", 4: "IMD_OK_HV",
      5: "CONT1_HV", 6: "CONT2_HV", 7: "INHIBIT_HV",
      16: "3V3_LV", 9: "GND", 15: "MHS_DRV", 14: "OKHS_DRV", 13: "IMD_OK_ISO",
      12: "CONT1_CLOSED", 11: "CONT2_CLOSED", 10: "IMD_INHIBIT"}, B,
     mpn="ISO7763FDWR", lcsc="C2871771", note="F = default-low outputs. 1500 Vrms / 2121 VDC working, 8 mm creepage")
C("100n", "3V3_HV", "GND_HV", B)
C("100n", "3V3_LV", "GND", B)
# ISO7721 (non-F, default high = UART idle). Channel A: side 2 -> side 1; B: side 1 -> side 2.
part("U", "Isolator:ISO7721DWV", "ISO7721DWVR", "Package_SO:SOIC-8_7.5x5.85mm_P1.27mm",
     {1: "3V3_HV", 4: "GND_HV", 2: "MCU_RX", 3: "MCU_TX",
      8: "3V3_LV", 5: "GND", 7: "UART_TO_IMD", 6: "UART_FROM_IMD_ISO"}, B,
     mpn="ISO7721DWVR", lcsc="C2864013", note="Non-F: idle-high UART. 8.5 mm creepage (DWV)")
C("100n", "3V3_HV", "GND_HV", B)
C("100n", "3V3_LV", "GND", B)

# --------------------------------------------------------------------------- LV logic I/O
B = "Economizer logic I/O"
R("100", "IMD_OK_ISO", "IMD_OK", B, note="Economizer adds 1k + ESD and a 100k pull-down")
R("100", "UART_FROM_IMD_ISO", "UART_FROM_IMD", B)
R("100k", "CONT1_CLOSED", "GND", B, note="Stand-alone: open = not closed")
R("100k", "CONT2_CLOSED", "GND", B)
R("100k", "IMD_INHIBIT", "GND", B, note="Stand-alone: open = measure")
R("100k", "UART_TO_IMD", "3V3_LV", B, note="UART idle high when no economizer")

# --------------------------------------------------------------------------- IR155-style outputs
for sig in ("MHS", "OKHS"):
    B = f"{sig} high-side output"
    part("Q", "Transistor_FET:2N7002", "2N7002", FP_SOT23, {1: f"{sig}_DRV", 2: "GND", 3: f"{sig}_PD"}, B,
         mpn="2N7002", lcsc="C8545")
    R("100k", f"{sig}_DRV", "GND", B, note="Off while LV logic is unpowered")
    R("10k", "VLV", f"{sig}_PG", B, note="Vgs = -VLV x 10/32: -3.8 V at 12 V, -12 V at the 38 V TVS clamp")
    R("22k", f"{sig}_PG", f"{sig}_PD", B)
    part("Q", "Transistor_FET:BSS84", "BSS84", FP_SOT23, {1: f"{sig}_PG", 2: "VLV", 3: f"{sig}_HS"}, B,
         mpn="LBSS84LT1G", lcsc="C8492", note="High-side switch, -50 V")
    R("470 1W", f"{sig}_HS", f"IMD_{sig}", B, FP_R2512,
      note="1 W: survives a dead short (0.55 W at 16 V). High = ~9.6 V into the IR155 2.2k load")
    part("D", "Device:D_TVS", "SMAJ24CA", FP_SMA, {1: f"IMD_{sig}", 2: "GND"}, B, mpn="SMAJ24CA (bidirectional)",
         lcsc="C113963", note="Harness ESD/transients, both polarities")

# =========================================================================== HV side
B = "HV logic supply"
part("U", "Regulator_Linear:LP5907MFX-3.3", "LP5907MFX-3.3", FP_SOT235,
     {1: "VISO_HV", 2: "GND_HV", 3: "VISO_HV", 4: None, 5: "3V3_HV"}, B, mpn="LP5907MFX-3.3/NOPB", lcsc="C80670",
     note="Low-noise LDO: MCU, ADC and the isolator HV sides")
C("1u", "VISO_HV", "GND_HV", B)
C("1u", "3V3_HV", "GND_HV", B)
R("10k", "VISO_HV", "VISO_MON", B, note="VISO/2 to MCU ADC: supply self-test")
R("10k", "VISO_MON", "GND_HV", B)
C("100n", "VISO_MON", "GND_HV", B)

B = "HV MCU"
part("U", "MCU_ST_STM32G0:STM32G031K8Tx", "STM32G031K8T6", "Package_QFP:LQFP-32_7x7mm_P0.8mm",
     {4: "3V3_HV", 5: "GND_HV", 6: "NRST_HV",
      7: "VISO_MON",                       # PA0  ADC_IN0
      9: "MCU_TX", 10: "MCU_RX",           # PA2/PA3 USART2 (AF1)
      11: "ADC_CS", 12: "ADC_SCK", 13: "ADC_MISO", 14: "ADC_MOSI",   # PA4 GPIO, PA5-7 SPI1 (AF0)
      15: "ADC_DRDY",                      # PB0 EXTI
      16: "R0P_EN", 17: "R0N_EN",          # PB1, PB2
      18: "MHS_HV",                        # PA8  TIM1_CH1 (AF2) PWM
      22: "OKHS_HV", 23: "IMD_OK_HV",      # PA11, PA12
      28: "CONT1_HV", 29: "CONT2_HV", 30: "INHIBIT_HV",   # PB4, PB5, PB6
      31: "LED_HB",                        # PB7 heartbeat
      24: "SWDIO_HV", 25: "SWCLK_HV",
      1: None, 2: None, 3: None, 8: None, 20: None, 26: None, 27: None, 32: None},
     B, mpn="STM32G031K8T6", lcsc="C432203", note="Same MCU as the economizer")
C("100n", "3V3_HV", "GND_HV", B)
C("4.7u", "3V3_HV", "GND_HV", B)
C("100n", "NRST_HV", "GND_HV", B)
R("1k", "LED_HB", "LED_HB_A", B)
part("D", "Device:LED", "yellow", FP_LED, {1: "GND_HV", 2: "LED_HB_A"}, B, note="HV-side heartbeat")
part("J", "Connector_Generic:Conn_01x05", "SWD (HV side)", "Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical",
     {1: "3V3_HV", 2: "SWDIO_HV", 3: "SWCLK_HV", 4: "NRST_HV", 5: "GND_HV"}, B, ref="J5",
     note="HV-REFERENCED. Program only with the HV connector unplugged")

B = "R0 bridge switches"
for side, top, bot, en in (("P", "HV_P", "E", "R0P_EN"), ("N", "E", "GND_HV", "R0N_EN")):
    # P: HV+ -> R0 string -> switch -> E.   N: E -> switch -> R0 string -> HV-.
    if side == "P":
        hv_string(top, "R0P_SW", "120k", 4, B, "R0P", note="R0 = 480k, HV+ to chassis")
        s1, s2 = "R0P_SW", bot
    else:
        s1, s2 = top, "R0N_SW"
        hv_string("R0N_SW", bot, "120k", 4, B, "R0N", note="R0 = 480k, chassis to HV-")
    part("U", "imd:TPSI2140-Q1", "TPSI2140QDWQRQ1", "imd:TI_DWQ0011A_SOIC-11_HV",
         {1: "VISO_HV", 3: en, 2: "GND_HV", 8: "GND_HV", 4: "GND_HV", 5: "GND_HV", 6: "GND_HV", 7: "GND_HV",
          11: s1, 9: s2, 10: None}, B, mpn="TPSI2140QDWQRQ1", lcsc="C19089118",
         note="1200 V, 130 ohm, 9 mA VDD when on. SM pin: thermal only, leave unconnected")
    C("100n", "VISO_HV", "GND_HV", B)
    R("100k", en, "GND_HV", B, note="Switch off during MCU reset")

B = "Sense dividers"
# Each: 4 x 1.2M HV string + 18k bottom (0.1 %). FS = 2.048 V x 4.818M/18k = 548 V.
# Designed for the 450 V class only (96S NMC, max 403 V): 112 V per 1206 at 450 V.
for name, top, node, dnp, note in (
        ("VBUS", "HV_P", "AIN_VBUS", False, "Pack voltage HV+ to HV-"),
        ("VE", "E", "AIN_VE", False, "Chassis (E) to HV-"),
        ("VKE", "KE", "AIN_VKE", False, "Chassis (KE) to HV-: E/KE mismatch = earth-connection fault")):
    hv_string(top, f"{name}_DIV", "1.2M", 4, B, name, note=note, dnp=dnp)
    R("18k", f"{name}_DIV", "GND_HV", B, dnp=dnp,
      note="1 %: channel gain matching comes from a one-time firmware calibration, not part tolerance")
    R("1k", f"{name}_DIV", node, B, dnp=dnp)
    C("100n", node, "GND_HV", B, note="Anti-alias, ~85 Hz with the divider", dnp=dnp)

B = "ADC"
part("U", "Analog_ADC:ADS1220xPW", "ADS1220IPWR", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm",
     {1: "ADC_SCK", 2: "ADC_CS", 3: "GND_HV", 4: "GND_HV", 5: "GND_HV",
      11: "AIN_VBUS", 10: "AIN_VE", 7: "AIN_VKE", 6: None,
      9: None, 8: None, 12: "3V3_HV", 13: "3V3_HV",
      14: "ADC_DRDY", 15: "ADC_MISO", 16: "ADC_MOSI"}, B, mpn="ADS1220IPWR", lcsc="C48263",
     note="24-bit, internal 2.048 V ref, PGA bypassed, single-ended vs AVSS. CLK=DGND -> internal osc. REFP0/REFN0/AIN3 floating (SBAS501D 9.1.5)")
C("100n", "3V3_HV", "GND_HV", B, note="AVDD")
C("100n", "3V3_HV", "GND_HV", B, note="DVDD")
C("1u", "3V3_HV", "GND_HV", B)
R("100k", "ADC_CS", "3V3_HV", B, note="ADC deselected during MCU reset")

B = "HV connectors"
# Micro-Fit 3.0 is rated 600 V. Middle position unloaded (6 mm between HV+ and HV-);
# its copper is removed on the PCB.
part("J", "Connector_Generic:Conn_01x03", "HV tap", "Connector_Molex:Molex_Micro-Fit_3.0_43650-0300_1x03_P3.00mm_Horizontal",
     {1: "HV_P", 2: None, 3: "GND_HV"}, B, ref="J3", mpn="Molex 43650-0300", lcsc="C503478",
     note="1 = HV+ (pack side, after pack fuse), 3 = HV-. Inline HV fuses in the harness")
part("J", "Connector_Generic:Conn_01x02", "Chassis E/KE", "Connector_Molex:Molex_Micro-Fit_3.0_43650-0200_1x02_P3.00mm_Horizontal",
     {1: "E", 2: "KE"}, B, ref="J4", mpn="Molex 43650-0200 (not at LCSC: Digi-Key/Mouser)",
     note="Two SEPARATE wires to chassis (different studs if possible). Never link E and KE on the board")

# --------------------------------------------------------------------------- stand-alone inputs
# Added last so existing reference designators do not change.
# CONT1/CONT2/INHIBIT from J2 are vehicle-level (12 V) signals: 22k + 10k divider onto the same
# nets the stacking header drives at 3.3 V, plus a BAT54S clamp to 3V3_LV / GND.
#   high above ~8 V (2.31 V = ISO7763 VIH at the node), low below ~3.4 V, 36 V load dump -> ~1.5 mA
#   into the clamp. Stacked, the economizer's 3.3 V through its 1k still gives ~3.0 V at the node.
B = "Stand-alone inputs"
for src, net in (("CONT1_J2", "CONT1_CLOSED"), ("CONT2_J2", "CONT2_CLOSED"), ("INHIBIT_J2", "IMD_INHIBIT")):
    R("22k", src, net, B, note="12 V-level input: divider top")
    R("10k", net, "GND", B, note="Divider bottom (with the 100k pull-down: 9.1k)")
    part("D", "Diode:BAT54S", "BAT54S", FP_SOT23, {1: "GND", 2: "3V3_LV", 3: net}, B,
         mpn="BAT54S", lcsc="C171786", note="Clamp to 3V3_LV / GND (load dump, ESD)")
# 3.3 V UART for a USB-serial adapter (calibration, status) when there is no economizer.
R("1k", "UART_IN_J2", "UART_TO_IMD", B, note="Series protection; a 5 V adapter is clamped by U11")
R("1k", "UART_FROM_IMD", "UART_OUT_J2", B, note="Series protection")
part("U", "Power_Protection:SRV05-4", "SRV05-4", "Package_TO_SOT_SMD:SOT-23-6",
     {1: "UART_IN_J2", 2: "GND", 3: "UART_OUT_J2", 4: None, 5: "3V3_LV", 6: None}, B,
     mpn="SRV05-4 (MSKSEMI)", lcsc="C2836319", note="ESD on the J2 UART pins")

# --------------------------------------------------------------------------- mechanical
# H1-H4 on the economizer's 69 x 59 grid; H5/H6 support the 34 mm overhang on the HV side.
for i in range(6):
    part("H", "Mechanical:MountingHole", "M3", "MountingHole:MountingHole_3.2mm_M3", {}, "Mechanical")


# --------------------------------------------------------------------------- JLCPCB part numbers
# Generic parts by (value, footprint). Checked on jlcpcb.com / lcsc.com 2026-11-26; "B" = Basic library.
LCSC_GENERIC = {
    ("100n", FP_C0603): "C14663",        # B  Yageo X7R 50 V
    ("100n 50V", FP_C0603): "C14663",    # B
    ("1u", FP_C0603): "C15849",          # B  Samsung X5R 50 V
    ("10n", FP_C0603): "C57112",         # B  X7R 50 V
    ("150n", FP_C0603): "C282674",       #    CCTC X7R 50 V
    ("4.7u", FP_C0603): "C19666",        # B  X5R 16 V
    ("10u 16V", FP_C0805): "C15850",     # B  X5R 25 V
    ("22u 16V", FP_C1206): "C12891",     #    Samsung X5R 25 V
    ("10u 50V", FP_C1206): "C13585",     #    Samsung X5R 50 V
    ("2.2u 50V", FP_C1206): "C50254",    # B  Samsung X7R 50 V
    ("100k", FP_R0603): "C25803",        # B
    ("10k", FP_R0603): "C25804",         # B
    ("1k", FP_R0603): "C21190",          # B
    ("22k", FP_R0603): "C31850",         # B
    ("100", FP_R0603): "C22775",         # B
    ("18k", FP_R0603): "C25810",         # B
    ("5.6k", FP_R0603): "C23189",        # B
    ("1.2M", FP_R1206): "C22107",        #    Uni-Royal 1 % 200 V
    ("120k", FP_R1206): "C17908",        #    Uni-Royal 1 % 200 V
    ("470 1W", FP_R2512): "C351718",     #    RESI 1 % 1 W
    ("green", FP_LED): "C72043",
    ("yellow", FP_LED): "C72038",
}
for _p in parts:
    if not _p["lcsc"]:
        _p["lcsc"] = LCSC_GENERIC.get((_p["value"], _p["footprint"]), "")


def nets():
    out = OrderedDict()
    for p in parts:
        for pin, net in p["pins"].items():
            if net:
                out.setdefault(net, []).append((p["ref"], pin))
    return out


def blocks():
    out = OrderedDict()
    for p in parts:
        out.setdefault(p["block"], []).append(p)
    return out


def check():
    refs = [p["ref"] for p in parts]
    dup = {r for r in refs if refs.count(r) > 1}
    assert not dup, f"duplicate refs {dup}"
    for net, conns in nets().items():
        if len(conns) < 2:
            raise AssertionError(f"net {net} has only one connection: {conns}")
    # Domain check: no part may touch both LV GND-domain and HV GND-domain nets except barrier parts.
    lv = {"GND", "VLV", "5V_LV", "3V3_LV", "12V_STACK", "12V_J2"}
    hv = {"GND_HV", "VISO_HV", "3V3_HV"}
    barrier = {p["ref"] for p in parts if p["lib_id"] in ("imd:UCC12050", "Isolator:ISO7763DW", "Isolator:ISO7721DWV")}
    for p in parts:
        ns = set(p["pins"].values())
        if ns & lv and ns & hv and p["ref"] not in barrier:
            raise AssertionError(f"{p['ref']} bridges LV and HV domains: {ns}")


check()

if __name__ == "__main__":
    for net, conns in nets().items():
        print(f"{net:<18} {' '.join(r + '.' + p for r, p in conns)}")
    print(len(parts), "parts,", len(nets()), "nets")
