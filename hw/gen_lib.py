"""Generate the project symbol library (imd.kicad_sym) and footprint library (imd.pretty).

Pinouts were taken from the TI datasheets:
  UCC12050     SNVSB38D (Jan 2021), DVE SOIC-16 wide, Table 5-1
  TPSI2140-Q1  SLVSFR3B (Jun 2023), DWQ SOIC-11, Table 5.1 and DWQ0011A land pattern (HV option)
"""
import os
import uuid

from sexpr import Sym as S, dump

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]
HIDE = [S("effects"), [S("font"), [S("size"), 1.27, 1.27]], [S("hide"), S("yes")]]


def uid(*key):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "imd-board/lib/" + "/".join(map(str, key))))


def prop(name, value, x, y, hide=False):
    return [S("property"), name, value, [S("at"), x, y, 0], HIDE if hide else FONT]


def pin(etype, num, name, x, y, ang, length=2.54):
    return [S("pin"), S(etype), S("line"), [S("at"), x, y, ang], [S("length"), length],
            [S("name"), name, FONT], [S("number"), str(num), FONT]]


def symbol(name, ref, value, footprint, datasheet, desc, w, h, pins):
    """Rectangle body of half-width w, half-height h; pins given as (etype,num,name,side,offset)."""
    body = [S("rectangle"), [S("start"), -w, h], [S("end"), w, -h],
            [S("stroke"), [S("width"), 0.254], [S("type"), S("default")]],
            [S("fill"), [S("type"), S("background")]]]
    # Dashed line down the middle marks the isolation barrier.
    barrier = [S("polyline"), [S("pts"), [S("xy"), 0, h], [S("xy"), 0, -h]],
               [S("stroke"), [S("width"), 0.254], [S("type"), S("dash")]], [S("fill"), [S("type"), S("none")]]]
    plist = []
    for etype, num, pname, side, off in pins:
        if side == "L":
            plist.append(pin(etype, num, pname, -w - 2.54, off, 0))
        elif side == "R":
            plist.append(pin(etype, num, pname, w + 2.54, off, 180))
    return [S("symbol"), name,
            [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
            prop("Reference", ref, 0, h + 1.27),
            prop("Value", value, 0, -h - 1.27),
            prop("Footprint", footprint, 0, 0, True),
            prop("Datasheet", datasheet, 0, 0, True),
            prop("Description", desc, 0, 0, True),
            [S("symbol"), f"{name}_0_1", body, barrier],
            [S("symbol"), f"{name}_1_1", *plist]]


SYMBOLS = [
    symbol("UCC12050", "U", "UCC12050DVER", "Package_SO:SOIC-16W_7.5x10.3mm_P1.27mm",
           "https://www.ti.com/lit/ds/symlink/ucc12050.pdf",
           "500 mW isolated DC/DC, 5 kVrms reinforced, 1697 VDC working, 5 V in, SEL-set 3.3/3.7/5.0/5.4 V out",
           12.7, 12.7, [
               ("power_in", 3, "VINP", "L", 10.16),
               ("input", 1, "EN", "L", 7.62),
               ("input", 4, "SYNC", "L", 5.08),
               ("open_collector", 5, "~{SYNC_OK}", "L", 2.54),
               ("passive", 6, "NC", "L", -2.54), ("passive", 7, "NC", "L", -5.08), ("passive", 8, "NC", "L", -7.62),
               ("power_in", 2, "GNDP", "L", -10.16),
               ("power_out", 14, "VISO", "R", 10.16),
               ("input", 13, "SEL", "R", 7.62),
               ("passive", 10, "NC", "R", 2.54), ("passive", 11, "NC", "R", 0), ("passive", 12, "NC", "R", -2.54),
               ("power_in", 9, "GNDS", "R", -5.08), ("power_in", 15, "GNDS", "R", -7.62),
               ("power_in", 16, "GNDS", "R", -10.16),
           ]),
    symbol("TPSI2140-Q1", "U", "TPSI2140QDWQRQ1", "imd:TI_DWQ0011A_SOIC-11_HV",
           "https://www.ti.com/lit/ds/symlink/tpsi2140-q1.pdf",
           "1200 V 50 mA automotive isolated switch, avalanche rated, no secondary supply needed",
           10.16, 12.7, [
               ("power_in", 1, "VDD", "L", 7.62),
               ("input", 3, "EN", "L", 5.08),
               ("passive", 4, "NC/GND", "L", 0), ("passive", 5, "NC/GND", "L", -2.54),
               ("passive", 6, "NC/GND", "L", -5.08), ("passive", 7, "NC/GND", "L", -7.62),
               ("power_in", 2, "GND", "L", 2.54),
               ("passive", 8, "GND", "L", -10.16),
               ("passive", 11, "S1", "R", 7.62),
               ("no_connect", 10, "SM", "R", 0),
               ("passive", 9, "S2", "R", -7.62),
           ]),
]


def write_symbols():
    lib = [S("kicad_symbol_lib"), [S("version"), 20241209], [S("generator"), "imd_gen"],
           [S("generator_version"), "1.0"], *SYMBOLS]
    with open(os.path.join(HERE, "imd.kicad_sym"), "w", encoding="utf-8") as f:
        f.write(dump(lib) + "\n")


def line(x1, y1, x2, y2, layer, w=0.12, key=""):
    return [S("fp_line"), [S("start"), x1, y1], [S("end"), x2, y2],
            [S("stroke"), [S("width"), w], [S("type"), S("solid")]], [S("layer"), layer],
            [S("uuid"), uid("ln", key, x1, y1, x2, y2, layer)]]


def rect(x1, y1, x2, y2, layer, w, key):
    return [line(x1, y1, x2, y1, layer, w, key), line(x2, y1, x2, y2, layer, w, key),
            line(x2, y2, x1, y2, layer, w, key), line(x1, y2, x1, y1, layer, w, key)]


def dwq0011a():
    """TI DWQ0011A (SOIC-11, TPSI2140-Q1), 'HV / isolation option' land pattern.

    Pads 1-8: 1.65 x 0.6 at 1.27 pitch. Pads 9-11: 1.65 x 1.15 at 4.173 pitch.
    Pad centres 9.75 mm apart (gives 8.1 mm clearance/creepage primary-secondary).
    """
    name = "TI_DWQ0011A_SOIC-11_HV"
    pads = []
    cx = 9.75 / 2
    for i in range(8):
        y = round(-3.5 * 1.27 + i * 1.27, 4)
        pads.append([S("pad"), str(i + 1), S("smd"), S("roundrect"), [S("at"), -cx, y], [S("size"), 1.65, 0.6],
                     [S("layers"), "F.Cu", "F.Paste", "F.Mask"], [S("roundrect_rratio"), 0.125],
                     [S("uuid"), uid(name, "pad", i + 1)]])
    for num, y in ((11, -4.173), (10, 0.0), (9, 4.173)):
        pads.append([S("pad"), str(num), S("smd"), S("roundrect"), [S("at"), cx, y], [S("size"), 1.65, 1.15],
                     [S("layers"), "F.Cu", "F.Paste", "F.Mask"], [S("roundrect_rratio"), 0.065],
                     [S("uuid"), uid(name, "pad", num)]])
    bx, by = 3.75, 5.15          # body 7.5 x 10.3
    fp = [S("footprint"), name, [S("version"), 20241229], [S("generator"), "imd_gen"], [S("layer"), "F.Cu"],
          [S("descr"), "TI DWQ0011A SOIC-11 (TPSI2140-Q1), HV/isolation land pattern, 9.75 mm pad row spacing"],
          [S("tags"), "SOIC-11 DWQ TPSI2140"],
          [S("property"), "Reference", "REF**", [S("at"), 0, -6.4, 0], [S("layer"), "F.SilkS"],
           [S("uuid"), uid(name, "ref")], [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]],
          [S("property"), "Value", name, [S("at"), 0, 6.4, 0], [S("layer"), "F.Fab"],
           [S("uuid"), uid(name, "val")], [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]],
          [S("attr"), S("smd")],
          *rect(-bx, -by, bx, by, "F.Fab", 0.1, "fab"),
          line(-bx + 0.1, -by - 0.11, bx - 0.1, -by - 0.11, "F.SilkS", 0.12, "s1"),
          line(-bx + 0.1, by + 0.11, bx - 0.1, by + 0.11, "F.SilkS", 0.12, "s2"),
          line(-5.95, -4.445, -5.95, -5.0, "F.SilkS", 0.12, "pin1"),
          *rect(-5.95, -5.55, 5.95, 5.55, "F.CrtYd", 0.05, "crt"),
          *pads,
          [S("model"), "${KICAD10_3DMODEL_DIR}/Package_SO.3dshapes/SOIC-16W_7.5x10.3mm_P1.27mm.step",
           [S("offset"), [S("xyz"), 0, 0, 0]], [S("scale"), [S("xyz"), 1, 1, 1]], [S("rotate"), [S("xyz"), 0, 0, 0]]]]
    d = os.path.join(HERE, "imd.pretty")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".kicad_mod"), "w", encoding="utf-8") as f:
        f.write(dump(fp) + "\n")


def write_lib_tables():
    with open(os.path.join(HERE, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n  (version 7)\n  (lib (name "imd")(type "KiCad")(uri "${KIPRJMOD}/imd.kicad_sym")(options "")(descr "IMD board parts"))\n)\n')
    with open(os.path.join(HERE, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n  (version 7)\n  (lib (name "imd")(type "KiCad")(uri "${KIPRJMOD}/imd.pretty")(options "")(descr "IMD board footprints"))\n)\n')


if __name__ == "__main__":
    write_symbols()
    dwq0011a()
    write_lib_tables()
    print("wrote imd.kicad_sym, imd.pretty (DWQ0011A), lib tables")
