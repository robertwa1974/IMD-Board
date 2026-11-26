"""Draw imd.kicad_sch from design.py.

Every connected pin gets a short wire stub ending in a net label; unused pins get a
no-connect flag. Parts are grouped into framed blocks (one per design.py block).
"""
import datetime
import os
import uuid

import design
import kilib
from sexpr import Sym as S, dump, find, find_all

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "imd"
ROOT_UUID = str(uuid.uuid5(uuid.NAMESPACE_URL, "imd-board/root"))
GRID = 1.27
STUB = 2.54
CHAR_W = 1.05         # approx label text width per character (1.27 mm font)
FONT = [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]


def uid(*key):
    """Deterministic UUIDs so regenerating keeps KiCad's links stable."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "imd-board/" + "/".join(map(str, key))))


def snap(v):
    return round(round(v / GRID) * GRID, 4)


def label_len(net):
    return len(net) * CHAR_W + 2.0


def pin_dir(angle):
    """Direction (dx, dy) in schematic coords (y down) pointing away from the body."""
    a = int(round(angle)) % 360
    return {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[a]


def part_extent(p, pins):
    """Bounding box (left, top, right, bottom) relative to symbol origin, schematic coords."""
    xs, ys = [0.0], [0.0]
    for num, (name, etype, px, py, ang, unit) in pins.items():
        x, y = px, -py
        net = p["pins"].get(num)
        dx, dy = pin_dir(ang)
        reach = STUB + (label_len(net) if net else 1.0)
        xs += [x, x + dx * reach]
        ys += [y, y + dy * reach]
    left, right = min(xs), max(xs)
    top, bottom = min(ys), max(ys)
    # Room for reference/value text on small two-pin parts.
    if len(pins) <= 3:
        right = max(right, 9.0)
    return left - 2.0, top - 3.0, right + 2.0, bottom + 3.0


def layout():
    """Place parts within blocks (row packing), then blocks on the sheet (shelf packing)."""
    blocks = design.blocks()
    block_w = 135.0
    placed_blocks = []
    for name, plist in blocks.items():
        x = y = 0.0
        row_h = 0.0
        pos = []
        for p in plist:
            pins = kilib.pins(p["lib_id"]) if p["pins"] or p["lib_id"] != "Mechanical:MountingHole" else {}
            l, t, r, b = part_extent(p, pins)
            w, h = r - l, b - t
            if x > 0 and x + w > block_w:
                x = 0.0
                y += row_h
                row_h = 0.0
            pos.append((p, x - l, y - t))
            x += w
            row_h = max(row_h, h)
        bw = block_w if len(plist) > 3 else max(x, 40.0)
        placed_blocks.append((name, pos, bw, y + row_h + 8.0))

    sheet_w, margin = 594.0, 12.0           # A2 landscape
    bx = by = margin
    shelf_h = 0.0
    out = []
    for name, pos, bw, bh in placed_blocks:
        if bx + bw > sheet_w - margin:
            bx = margin
            by += shelf_h + 6.0
            shelf_h = 0.0
        out.append((name, bx, by, bw, bh, [(p, snap(bx + 2 + x), snap(by + 8 + y)) for p, x, y in pos]))
        bx += bw + 8.0
        shelf_h = max(shelf_h, bh)
    return out, by + shelf_h


def place_property(lib_prop, value, X, Y, hide=False):
    at = find(lib_prop, "at")
    eff = find(lib_prop, "effects")
    eff = [x for x in (eff if eff else FONT) if not (isinstance(x, list) and x[0] == "hide")]
    hidden = hide or bool(find(lib_prop, "hide")) or bool(find(find(lib_prop, "effects") or [], "hide"))
    out = [S("property"), lib_prop[1], value,
           [S("at"), round(X + float(at[1]), 4), round(Y - float(at[2]), 4), float(at[3]) if len(at) > 3 else 0]]
    if hidden:
        out.append([S("hide"), S("yes")])
    out.append(eff)
    return out


def build():
    blocks, height = layout()
    paper = "A2" if height < 400 else "A1"
    items = []
    lib_syms = {}

    for bname, bx, by, bw, bh, plist in blocks:
        items.append([S("rectangle"), [S("start"), bx, by], [S("end"), bx + bw, by + bh],
                      [S("stroke"), [S("width"), 0.2], [S("type"), S("dash")]], [S("fill"), [S("type"), S("none")]],
                      [S("uuid"), uid("blk", bname)]])
        items.append([S("text"), bname, [S("exclude_from_sim"), S("no")], [S("at"), bx + 2, by + 5, 0],
                      [S("effects"), [S("font"), [S("size"), 2.5, 2.5], [S("bold"), S("yes")]],
                       [S("justify"), S("left"), S("bottom")]], [S("uuid"), uid("blktxt", bname)]])
        for p, X, Y in plist:
            lid = p["lib_id"]
            if lid not in lib_syms:
                lib_syms[lid] = kilib.get_symbol(lid)
            sym = lib_syms[lid]
            pins = kilib.pins(lid)
            unknown = set(p["pins"]) - set(pins)
            assert not unknown, f"{p['ref']}: pins {unknown} not in symbol {lid}"

            props = {pr[1]: pr for pr in find_all(sym, "property")}
            inst = [S("symbol"), [S("lib_id"), lid], [S("at"), X, Y, 0], [S("unit"), 1],
                    [S("exclude_from_sim"), S("no")],
                    [S("in_bom"), S("no") if p["ref"].startswith(("#", "H")) else S("yes")],
                    [S("on_board"), S("no") if p["ref"].startswith("#") else S("yes")],
                    [S("dnp"), S("yes") if p["dnp"] else S("no")],
                    [S("uuid"), uid("sym", p["ref"])]]
            values = {"Reference": p["ref"], "Value": p["value"], "Footprint": p["footprint"]}
            for key in ("Reference", "Value", "Footprint", "Datasheet", "Description"):
                if key in props:
                    v = values.get(key, props[key][2])
                    inst.append(place_property(props[key], v, X, Y,
                                               hide=key in ("Footprint", "Datasheet", "Description")))
            for key, val in (("MPN", p["mpn"]), ("LCSC", p["lcsc"]), ("Notes", p["note"])):
                if val:
                    inst.append([S("property"), key, val, [S("at"), X, Y, 0], [S("hide"), S("yes")],
                                 [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]])
            for num in pins:
                inst.append([S("pin"), num, [S("uuid"), uid("pin", p["ref"], num)]])
            inst.append([S("instances"), [S("project"), PROJECT,
                                          [S("path"), "/" + ROOT_UUID, [S("reference"), p["ref"]], [S("unit"), 1]]]])
            items.append(inst)

            for num, (name, etype, px, py, ang, unit) in pins.items():
                ex, ey = round(X + px, 4), round(Y - py, 4)
                net = p["pins"].get(num)
                if not net:
                    if etype != "no_connect":
                        items.append([S("no_connect"), [S("at"), ex, ey], [S("uuid"), uid("nc", p["ref"], num)]])
                    continue
                dx, dy = pin_dir(ang)
                sx, sy = round(ex + dx * STUB, 4), round(ey + dy * STUB, 4)
                items.append([S("wire"), [S("pts"), [S("xy"), ex, ey], [S("xy"), sx, sy]],
                              [S("stroke"), [S("width"), 0], [S("type"), S("default")]],
                              [S("uuid"), uid("w", p["ref"], num)]])
                lang, just = {(-1, 0): (180, "right"), (1, 0): (0, "left"),
                              (0, -1): (90, "left"), (0, 1): (270, "right")}[(dx, dy)]
                # Global labels: net names without the "/" sheet prefix, matching the PCB.
                gjust = "right" if just == "right" else "left"
                items.append([S("global_label"), net, [S("shape"), S("passive")], [S("at"), sx, sy, lang],
                              [S("fields_autoplaced"), S("yes")],
                              [S("effects"), [S("font"), [S("size"), 1.27, 1.27]], [S("justify"), S(gjust)]],
                              [S("uuid"), uid("lbl", p["ref"], num)],
                              [S("property"), "Intersheetrefs", "${INTERSHEET_REFS}", [S("at"), sx, sy, 0],
                               [S("hide"), S("yes")], [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]]])

    today = datetime.date.today().isoformat()
    sch = [S("kicad_sch"), [S("version"), 20250114], [S("generator"), "eeschema"], [S("generator_version"), "9.0"],
           [S("uuid"), ROOT_UUID], [S("paper"), paper],
           [S("title_block"), [S("title"), "DIY insulation monitor (IMD), IR155-compatible outputs"], [S("date"), today],
            [S("rev"), "A"], [S("company"), "Open hardware"],
            [S("comment"), 1, "Generated by hw/gen_sch.py from hw/design.py - edit design.py, not this file"],
            [S("comment"), 2, "Switched-R0 bridge (TPSI2140) + ADS1220 on HV- domain; UCC12050 / ISO7763 / ISO7721 barrier"]],
           [S("lib_symbols"), *lib_syms.values()],
           *items,
           [S("sheet_instances"), [S("path"), "/", [S("page"), "1"]]],
           [S("embedded_fonts"), S("no")]]
    path = os.path.join(HERE, PROJECT + ".kicad_sch")
    with open(path, "w", encoding="utf-8") as f:
        f.write(dump(sch) + "\n")
    print(f"wrote {path} ({paper}, {len(design.parts)} parts, {len(design.nets())} nets)")


if __name__ == "__main__":
    build()
