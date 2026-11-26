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
    _counters[prefix] = _counters.get(prefix, 0) + 1
    return f"{prefix}{_counters[prefix]}"


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
          mpn="1206 HV-rated thick film (>=200 V/part), 1 %, e.g. Vishay CRHV / Yageo HV")


# =========================================================================== LV side
# --------------------------------------------------------------------------- connectors
B = "Stack header"
# Pin numbers match the economizer's J4 socket (economizer hw/design.py). UART names are from
# the signal's point of view, so there is no TX/RX ambiguity:
#   pin 9  UART_TO_IMD   = economizer TX (PB6) -> IMD RX
#   pin 10 UART_FROM_IMD = IMD TX -> economizer RX (PB7)
part("J", "Connector_Generic:Conn_02x06_Odd_Even", "Stack header 2x6",
     "Connector_PinHeader_2.54mm:PinHeader_2x06_P2.54mm_Vertical",
     {1: "12V_IN", 2: "GND", 3: "IMD_OK", 4: "GND", 5: "CONT1_CLOSED", 6: "CONT2_CLOSED",
      7: "IMD_INHIBIT", 8: "GND", 9: "UART_TO_IMD", 10: "UART_FROM_IMD", 11: "IMD_MHS", 12: "IMD_OKHS"},
     B, ref="J1", mpn="2x6 elevated male header, 2.54 mm (length to suit 15 mm standoffs + 8.5 mm socket)",
     note="Footprint on top side at the same XY/rotation as economizer J4; header inserted from below")

B = "Stand-alone connector"
part("J", "Connector_Generic:Conn_01x04", "Micro-Fit 1x4",
     "Connector_Molex:Molex_Micro-Fit_3.0_43650-0400_1x04_P3.00mm_Horizontal",
     {1: "12V_IN", 2: "GND", 3: "IMD_MHS", 4: "IMD_OKHS"}, B, ref="J2", mpn="Molex 43650-0400",
     note="Only when NOT stacked: permanent 12 V, GND, IR155-style MHS/OKHS to VCU")

# --------------------------------------------------------------------------- LV power
B = "LV power input"
part("D", "Diode:SS14", "SS14", FP_SMA, {1: "VLV", 2: "12V_IN"}, B, mpn="SS14", lcsc="C2480",
     note="Reverse polarity (stand-alone use); ~0.1 A load")
part("D", "Device:D_Zener", "SMAJ24A", FP_SMA, {1: "VLV", 2: "GND"}, B, mpn="SMAJ24A (unidirectional TVS)",
     note="Load-dump clamp for stand-alone use (economizer already clamps when stacked)")
C("10u 50V", "VLV", "GND", B, FP_C1206)
C("100n 50V", "VLV", "GND", B)
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "VLV"}, B, ref="#FLG01")
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "GND"}, B, ref="#FLG02")
part("#FLG", "power:PWR_FLAG", "PWR_FLAG", "", {1: "12V_IN"}, B, ref="#FLG03")

B = "LV 5 V buck"
part("U", "Regulator_Switching:LMR14206", "LMR14206XMK", FP_SOT236,
     {1: "BUCK_CB", 2: "GND", 3: "BUCK_FB", 4: "BUCK_SHDN", 5: "VLV", 6: "BUCK_SW"}, B,
     mpn="LMR14206XMKE/NOPB", note="4.5-42 V in, 1.25 MHz, 0.6 A. Values from datasheet Fig. 10 (5 V out)")
R("100k", "VLV", "BUCK_SHDN", B, note="Datasheet: 100k from supply when SHDN may see >42 V")
C("10n", "BUCK_SHDN", "GND", B, note="Soft start")
C("2.2u 50V", "VLV", "GND", B, FP_C1206, note="CIN, at VIN/GND pins")
C("150n", "BUCK_CB", "BUCK_SW", B, note="CBOOT >= 0.15 uF")
part("D", "Diode:SS16", "SS16", FP_SMA, {1: "BUCK_SW", 2: "GND"}, B, mpn="SS16", lcsc="C64981",
     note="Catch diode, 60 V 1 A (datasheet example)")
part("L", "Device:L", "15u", "Inductor_SMD:L_Bourns_SRN6045TA", {1: "BUCK_SW", 2: "5V_LV"}, B,
     mpn="Bourns SRN6045TA-150M", note=">=1.1 A saturation so a short does not saturate it")
R("5.62k", "5V_LV", "BUCK_FB", B, note="Vout = 0.765 V x (1 + 5.62k/1.02k) = 4.98 V")
R("1.02k", "BUCK_FB", "GND", B)
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
      9: "GND_HV", 15: "GND_HV", 16: "GND_HV"}, B, mpn="UCC12050DVER (UCC12051QDVERQ1 automotive)",
     note="SEL=VISO -> 5.0 V. SYNC=GNDP (internal osc). NC 6-8 to GNDP, NC 10-12 to GNDS per datasheet")
R("10k", "5V_LV", "ISO_EN", B, note="EN high = on")
C("10u 16V", "5V_LV", "GND", B, FP_C0805, note="At VINP pin 3 / GNDP pin 2")
C("100n", "5V_LV", "GND", B)
C("10u 16V", "VISO_HV", "GND_HV", B, FP_C0805, note="At VISO pin 14 / GNDS pin 15")
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
     mpn="ISO7763FDWR", note="F = default-low outputs. 1500 Vrms / 2121 VDC working, 8 mm creepage")
C("100n", "3V3_HV", "GND_HV", B)
C("100n", "3V3_LV", "GND", B)
# ISO7721 (non-F, default high = UART idle). Channel A: side 2 -> side 1; B: side 1 -> side 2.
part("U", "Isolator:ISO7721DWV", "ISO7721DWVR", "Package_SO:SOIC-8_7.5x5.85mm_P1.27mm",
     {1: "3V3_HV", 4: "GND_HV", 2: "MCU_RX", 3: "MCU_TX",
      8: "3V3_LV", 5: "GND", 7: "UART_TO_IMD", 6: "UART_FROM_IMD_ISO"}, B,
     mpn="ISO7721DWVR", note="Non-F: idle-high UART. 8.5 mm creepage (DWV)")
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
         mpn="BSS84", lcsc="C8492", note="High-side switch, -50 V")
    R("470", f"{sig}_HS", f"IMD_{sig}", B, FP_R2512,
      note="1 W: survives a dead short (0.55 W at 16 V). High = ~9.6 V into the IR155 2.2k load")
    part("D", "Device:D_TVS", "SMAJ24CA", FP_SMA, {1: f"IMD_{sig}", 2: "GND"}, B, mpn="SMAJ24CA (bidirectional)",
         note="Harness ESD/transients, both polarities")

# =========================================================================== HV side
B = "HV logic supply"
part("U", "Regulator_Linear:LP5907MFX-3.3", "LP5907MFX-3.3", FP_SOT235,
     {1: "VISO_HV", 2: "GND_HV", 3: "VISO_HV", 4: None, 5: "3V3_HV"}, B, mpn="LP5907MFX-3.3/NOPB",
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
        hv_string(top, "R0P_SW", "100k", 5, B, "R0P", note="R0 = 500k, HV+ to chassis")
        s1, s2 = "R0P_SW", bot
    else:
        s1, s2 = top, "R0N_SW"
        hv_string("R0N_SW", bot, "100k", 5, B, "R0N", note="R0 = 500k, chassis to HV-")
    part("U", "imd:TPSI2140-Q1", "TPSI2140QDWQRQ1", "imd:TI_DWQ0011A_SOIC-11_HV",
         {1: "VISO_HV", 3: en, 2: "GND_HV", 8: "GND_HV", 4: "GND_HV", 5: "GND_HV", 6: "GND_HV", 7: "GND_HV",
          11: s1, 9: s2, 10: None}, B, mpn="TPSI2140QDWQRQ1",
         note="1200 V, 130 ohm, 9 mA VDD when on. SM pin: thermal only, leave unconnected")
    C("100n", "VISO_HV", "GND_HV", B)
    R("100k", en, "GND_HV", B, note="Switch off during MCU reset")

B = "Sense dividers"
# Each: 5 x 1M HV string + 18k bottom (0.1 %). FS = 2.048 V x 5.018M/18k = 571 V.
# For the 800 V option change the 18k bottoms to 12k (FS 856 V).
for name, top, node, dnp, note in (
        ("VBUS", "HV_P", "AIN_VBUS", False, "Pack voltage HV+ to HV-"),
        ("VE", "E", "AIN_VE", False, "Chassis (E) to HV-"),
        ("VKE", "KE", "AIN_VKE", False, "Chassis (KE) to HV-: E/KE mismatch = earth-connection fault"),
        ("VBB", "BUS_P", "AIN_VBB", True, "Optional: bus side of the contactor (weld detect)")):
    hv_string(top, f"{name}_DIV", "1M", 5, B, name, note=note, dnp=dnp)
    R("18k 0.1%", f"{name}_DIV", "GND_HV", B, mpn="0.1 % 25 ppm thin film", dnp=dnp)
    R("1k", f"{name}_DIV", node, B, dnp=dnp)
    C("100n", node, "GND_HV", B, note="Anti-alias, ~85 Hz with the divider", dnp=dnp)

B = "ADC"
part("U", "Analog_ADC:ADS1220xPW", "ADS1220IPWR", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm",
     {1: "ADC_SCK", 2: "ADC_CS", 3: "GND_HV", 4: "GND_HV", 5: "GND_HV",
      11: "AIN_VBUS", 10: "AIN_VE", 7: "AIN_VKE", 6: "AIN_VBB",
      9: None, 8: None, 12: "3V3_HV", 13: "3V3_HV",
      14: "ADC_DRDY", 15: "ADC_MISO", 16: "ADC_MOSI"}, B, mpn="ADS1220IPWR",
     note="24-bit, internal 2.048 V ref, PGA bypassed, single-ended vs AVSS. CLK=DGND -> internal osc. REFP0/REFN0 floating (SBAS501D 9.1.5: lowest leakage)")
C("100n", "3V3_HV", "GND_HV", B, note="AVDD")
C("100n", "3V3_HV", "GND_HV", B, note="DVDD")
C("1u", "3V3_HV", "GND_HV", B)
R("100k", "ADC_CS", "3V3_HV", B, note="ADC deselected during MCU reset")

B = "HV connectors"
# Micro-Fit 3.0 is rated 600 V. Only every other position is loaded (6 mm between used pins);
# remove the copper of positions 2 and 4 on the PCB.
part("J", "Connector_Generic:Conn_01x05", "HV tap", "Connector_Molex:Molex_Micro-Fit_3.0_43650-0500_1x05_P3.00mm_Horizontal",
     {1: "HV_P", 2: None, 3: "BUS_P", 4: None, 5: "GND_HV"}, B, ref="J3", mpn="Molex 43650-0500",
     note="1 = HV+ (pack side, after pack fuse), 3 = bus side (optional), 5 = HV-. Inline HV fuses in the harness")
part("J", "Connector_Generic:Conn_01x02", "Chassis E/KE", "Connector_Molex:Molex_Micro-Fit_3.0_43650-0200_1x02_P3.00mm_Horizontal",
     {1: "E", 2: "KE"}, B, ref="J4", mpn="Molex 43650-0200",
     note="Two SEPARATE wires to chassis (different studs if possible). Never link E and KE on the board")

# --------------------------------------------------------------------------- mechanical
for i in range(4):
    part("H", "Mechanical:MountingHole", "M3", "MountingHole:MountingHole_3.2mm_M3", {}, "Mechanical")


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
    lv = {"GND", "VLV", "5V_LV", "3V3_LV", "12V_IN"}
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
