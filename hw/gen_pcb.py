"""Build imd.kicad_pcb from design.py (run with KiCad's python.exe).

Stage "place":  footprints, nets, outline (notch + isolation slots), HV tracks (locked), netclasses
                -> imd.kicad_pcb + imd.dsn for Freerouting.
Stage "finish": import Freerouting's imd.ses, add GND (LV) and GND_HV pours, fill, save.

Floorplan (mm, origin top-left, 76 x 66, same outline and M3 holes as the economizer):
  LV domain x < 31 (left): stacking header bottom-left (same XY as economizer J4), stand-alone
    connector on the left edge, buck top-left, MHS/OKHS drivers, input protection.
  Barrier column x = 35: UCC12050, ISO7763F, ISO7721 straddle it; 8 mm copper-free gap x 31..39,
    milled slots under the barrier packages.
  HV domain x > 33: zone A (x 39..59) LDO, MCU, ADC, sense low ends; zone B: two TPSI2140 with
    their switch pads facing right, five HV strings as horizontal rows (HV end right, x ~ 92),
    HV tap (HV+/HV-) and E/KE connectors on the right edge of the 30 mm overhang.
  Notch over economizer J1 (latch access): x 25..51, y 0..8.
  H2/H4 (economizer grid) and H5/H6 (overhang support) are in the HV domain: NYLON hardware.
"""
import math
import os
import shutil
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design  # noqa: E402
from gen_sch import uid  # noqa: E402

FPDIR = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
PCB = os.path.join(HERE, "imd.kicad_pcb")
PLACED = os.path.join(HERE, "imd_placed.kicad_pcb")
W, H = 106.0, 66.0          # 76 x 66 over the economizer + 30 mm overhang (HV side)
NOTCH = (25.0, 51.0, 8.0)          # x0, x1, depth
BARRIER_X = 35.0
LV_MAX_X, HV_MIN_X = 31.0, 39.0

HV_NETS = ["HV_P", "E", "KE", "R0P_SW", "R0N_SW"] + [f"{s}_{i}" for s in ("R0P", "R0N", "VBUS", "VE", "VKE")
                                                     for i in (1, 2, 3)]
POWER_NETS = ["12V_STACK", "12V_J2", "VLV", "BUCK_SW", "5V_LV", "IMD_MHS", "IMD_OKHS", "MHS_HS", "OKHS_HS"]


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


# ---------------------------------------------------------------- placement: ref -> (x, y, rot)
PLACE = {
    # ---- LV: connectors and input
    "J1": (9.5, 56.0, 90),           # same XY/rotation as economizer J4 (header inserted from below)
    "J2": (9.46, 44.0, 90),          # stand-alone Micro-Fit, mating face to the left edge
    "D2": (19.0, 41.2, 0), "D1": (19.0, 45.0, 0), "D8": (19.0, 48.8, 0), "C1": (13.5, 42.5, 90), "C2": (13.5, 46.6, 90),
    # ---- LV: 5 V buck, top-left (H1 keep-out at the corner, J1-latch notch from x 25)
    "U1": (11.5, 5.0, 0), "C5": (11.5, 1.9, 0), "L1": (18.5, 4.6, 0), "D3": (18.0, 10.2, 0),
    "C4": (8.3, 10.0, 90), "R1": (11.5, 9.0, 0), "C3": (11.5, 10.8, 0),
    "R2": (11.5, 12.6, 0), "R3": (11.5, 14.4, 0),
    "C6": (23.3, 10.5, 90), "C7": (26.3, 10.5, 90),
    # ---- LV: 3.3 V, power LED
    "U2": (19.0, 15.5, 0), "C8": (15.5, 15.5, 90), "C9": (22.5, 15.5, 90),
    "R4": (4.0, 15.0, 90), "D4": (4.0, 18.5, 90),
    # ---- LV: barrier-side parts (U3/U4/U5 LV pins at x ~ 30.3)
    "C10": (27.3, 15.3, 270), "C11": (24.6, 15.3, 90), "R5": (24.5, 18.8, 0),
    "C15": (26.4, 21.5, 0), "R6": (26.4, 23.3, 0), "R11": (26.4, 25.1, 0),
    "R8": (23.3, 23.3, 0), "R9": (23.3, 25.1, 0), "R10": (23.3, 26.9, 0),
    "C17": (26.4, 41.6, 0), "R7": (26.4, 43.4, 0),
    # ---- LV: MHS driver (y 20..30)
    "R15": (6.0, 23.5, 0), "D5": (6.0, 27.8, 0), "Q2": (12.5, 23.5, 0), "R13": (12.5, 20.6, 0),
    "R14": (12.5, 26.5, 0), "Q1": (16.5, 23.5, 0), "R12": (16.5, 26.5, 0),
    # ---- LV: OKHS driver (y 30..39)
    "Q4": (13.5, 32.0, 0), "R17": (13.5, 35.0, 0), "R18": (13.5, 36.8, 0),
    "R19": (20.5, 32.5, 0), "D6": (20.5, 37.0, 0), "Q3": (26.5, 33.0, 90), "R16": (26.5, 36.5, 0),
    # ---- barrier
    "U3": (BARRIER_X, 14.5, 0),      # UCC12050: primary (pins 1-8) left = LV
    "U4": (BARRIER_X, 27.0, 180),    # ISO7763F: side 1 (HV) to the right
    "U5": (BARRIER_X, 37.5, 180),    # ISO7721: side 1 (HV) to the right
    # ---- HV zone A (x 39..59): supply, MCU, ADC, sense low ends
    "C12": (42.4, 11.5, 90), "C13": (42.4, 15.5, 90), "C14": (42.4, 23.0, 90), "C16": (42.4, 34.0, 90),
    "U6": (47.0, 11.0, 0), "C18": (47.0, 14.6, 0), "C19": (51.5, 11.0, 90),
    "R20": (55.0, 11.0, 0), "R21": (55.0, 12.8, 0), "C20": (55.0, 14.6, 0),
    "U7": (50.0, 25.0, 0), "C21": (46.0, 18.6, 0), "C22": (50.0, 18.6, 0), "C23": (53.5, 32.5, 0),
    "U10": (50.0, 37.0, 0), "C29": (46.5, 41.4, 0), "C30": (50.0, 41.4, 0), "C31": (53.5, 41.4, 0),
    "R51": (56.0, 35.5, 90),
    "R37": (47.0, 45.0, 0), "R38": (47.0, 46.8, 0), "C26": (47.0, 48.6, 0),    # VBUS
    "R43": (51.0, 45.0, 0), "R44": (51.0, 46.8, 0), "C27": (51.0, 48.6, 0),    # VE
    "R49": (55.0, 45.0, 0), "R50": (55.0, 46.8, 0), "C28": (55.0, 48.6, 0),    # VKE
    "J5": (43.0, 52.0, 90), "R22": (54.0, 56.0, 0), "D7": (54.0, 58.0, 0),
    "C24": (57.0, 18.4, 90), "R27": (57.0, 22.0, 90),
    "C25": (57.0, 32.0, 90), "R32": (57.0, 41.0, 90),
    # ---- HV zone B: switches (switch pads face right), strings as rows, connectors on the right edge
    "U8": (65.0, 21.2, 0),           # R0P: S1 (pin 11, top-right) = R0P_SW, S2 (pin 9) = E
    "U9": (65.0, 34.5, 0),           # R0N: S1 (pin 11, top-right) = E, S2 (pin 9) = R0N_SW
    "J3": (96.54, 12.0, 270),        # 1 = HV+ (y 12), 2 unloaded, 3 = HV- (y 18); mating face right
    "J4": (96.54, 28.0, 270),        # 1 = E (y 28), 2 = KE (y 31)
    # ---- mounting holes: economizer grid + overhang support
    "H1": (3.5, 3.5, 0), "H2": (72.5, 3.5, 0), "H3": (3.5, H - 3.5, 0), "H4": (72.5, H - 3.5, 0),
    "H5": (W - 3.5, 3.5, 0), "H6": (W - 3.5, H - 3.5, 0),
}
ROW_X = (90.6, 85.9, 81.2, 76.5)    # HV end on the right (x ~ 92), low end on the left (x ~ 75)
ROW_Y = {"VBUS": 12.0, "R0P": 17.0, "VE": 23.0, "VKE": 33.0, "R0N": 38.7}


def _row(refs, y, rot=180):
    """refs[0] at the right (HV end). rot 180 puts pad 1 on the right."""
    return {r: (x, y, rot) for r, x in zip(refs, ROW_X)}


PLACE.update(_row(["R33", "R34", "R35", "R36"], ROW_Y["VBUS"]))
PLACE.update(_row(["R23", "R24", "R25", "R26"], ROW_Y["R0P"]))
PLACE.update(_row(["R39", "R40", "R41", "R42"], ROW_Y["VE"]))
PLACE.update(_row(["R45", "R46", "R47", "R48"], ROW_Y["VKE"]))
# R0N runs the other way: R0N_SW (R28 pad 1) on the left next to U9 S2, GND_HV end on the right.
PLACE.update({r: (x, ROW_Y["R0N"], 0) for r, x in zip(["R28", "R29", "R30", "R31"], reversed(ROW_X))})


def placements():
    missing = [p["ref"] for p in design.parts if p["footprint"] and p["ref"] not in PLACE]
    assert not missing, f"unplaced: {missing}"
    return PLACE


# ---------------------------------------------------------------- board helpers
def load_fp(fpid):
    lib, name = fpid.split(":")
    path = os.path.join(HERE, "imd.pretty") if lib == "imd" else os.path.join(FPDIR, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def setup_rules(board):
    ds = board.GetDesignSettings()
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_MinClearance = mm(0.15)
    ds.m_ViasMinSize = mm(0.5)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_CopperEdgeClearance = mm(0.3)
    ds.m_HoleClearance = mm(0.25)
    ds.m_HoleToHoleMin = mm(0.5)
    ns = ds.m_NetSettings
    ns.ClearNetclassPatternAssignments()
    dflt = ns.GetDefaultNetclass()
    dflt.SetClearance(mm(0.2)); dflt.SetTrackWidth(mm(0.25))
    dflt.SetViaDiameter(mm(0.6)); dflt.SetViaDrill(mm(0.3))
    pwr = pcbnew.NETCLASS("Power")
    pwr.SetClearance(mm(0.25)); pwr.SetTrackWidth(mm(0.6))
    pwr.SetViaDiameter(mm(0.8)); pwr.SetViaDrill(mm(0.4))
    ns.SetNetclass("Power", pwr)
    for n in POWER_NETS:
        ns.SetNetclassPatternAssignment(n, "Power")
    # HV nets: 2.5 mm to anything (IEC 60664-1 PD2, MG III, 500 V row). Between nodes of the same
    # string the DRC uses the custom rules in imd.kicad_dru instead.
    hv = pcbnew.NETCLASS("HV")
    hv.SetClearance(mm(2.5)); hv.SetTrackWidth(mm(0.5))
    hv.SetViaDiameter(mm(0.8)); hv.SetViaDrill(mm(0.4))
    ns.SetNetclass("HV", hv)
    for n in HV_NETS:
        ns.SetNetclassPatternAssignment(n, "HV")
    for name, net in (("Gnd", "GND"), ("GndHV", "GND_HV")):
        c = pcbnew.NETCLASS(name)
        c.SetClearance(mm(0.2)); c.SetTrackWidth(mm(0.4))
        c.SetViaDiameter(mm(0.7)); c.SetViaDrill(mm(0.35))
        ns.SetNetclass(name, c)
        ns.SetNetclassPatternAssignment(net, name)
    ns.RecomputeEffectiveNetclasses()


def seg(board, a, b, layer=pcbnew.Edge_Cuts, w=0.1):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pt(*a)); s.SetEnd(pt(*b))
    s.SetLayer(layer); s.SetWidth(mm(w))
    board.Add(s)


def arc(board, s_, m_, e_):
    a = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
    a.SetArcGeometry(pt(*s_), pt(*m_), pt(*e_))
    a.SetLayer(pcbnew.Edge_Cuts); a.SetWidth(mm(0.1))
    board.Add(a)


def outline(board):
    """76 x 66 with 2 mm corner radii and the J1-latch notch in the top edge."""
    r = 2.0
    k = r * (1 - 0.70710678)
    x0, x1, d = NOTCH
    seg(board, (r, 0), (x0, 0)); seg(board, (x0, 0), (x0, d)); seg(board, (x0, d), (x1, d))
    seg(board, (x1, d), (x1, 0)); seg(board, (x1, 0), (W - r, 0))
    seg(board, (W, r), (W, H - r)); seg(board, (W - r, H), (r, H)); seg(board, (0, H - r), (0, r))
    for s_, m_, e_ in (((0, r), (k, k), (r, 0)), ((W - r, 0), (W - k, k), (W, r)),
                       ((W, H - r), (W - k, H - k), (W - r, H)), ((r, H), (k, H - k), (0, H - r))):
        arc(board, s_, m_, e_)


def slot(board, x, y0, y1, w=1.5):
    """Milled isolation slot (stadium shape) centred on x from y0 to y1."""
    r = w / 2
    seg(board, (x - r, y0), (x - r, y1)); seg(board, (x + r, y0), (x + r, y1))
    arc(board, (x - r, y0), (x, y0 - r), (x + r, y0))
    arc(board, (x + r, y1), (x, y1 + r), (x - r, y1))


def silk(board, text, x, y, size=1.0, layer=None, rot=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetPosition(pt(x, y))
    t.SetLayer(layer if layer is not None else pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * 0.15))
    t.SetTextAngleDegrees(rot)
    if t.GetLayer() == pcbnew.B_SilkS:
        t.SetMirrored(True)
    board.Add(t)


def pad(board, ref, num):
    fp = board.FindFootprintByReference(ref)
    for p in fp.Pads():
        if p.GetNumber() == str(num):
            return p
    raise KeyError(f"{ref}.{num}")


def track(board, pts, net, width=0.5, layer=pcbnew.F_Cu):
    """Locked polyline through the given points (mm tuples or VECTOR2I)."""
    vs = [p if isinstance(p, pcbnew.VECTOR2I) else pt(*p) for p in pts]
    for a, b in zip(vs, vs[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(a); t.SetEnd(b)
        t.SetWidth(mm(width)); t.SetLayer(layer)
        t.SetNet(net); t.SetLocked(True)
        board.Add(t)


def xy(p):
    v = p.GetPosition()
    return pcbnew.ToMM(v.x), pcbnew.ToMM(v.y)


def chain(board, refs):
    """Join consecutive resistors of a string pad 2 -> next pad 1 with locked tracks."""
    for a, b in zip(refs, refs[1:]):
        pa, pb = pad(board, a, 2), pad(board, b, 1)
        track(board, [pa.GetPosition(), pb.GetPosition()], pa.GetNet())


def hv_routes(board):
    """All HV-net connections, hand-routed and locked (the autorouter only sees them as obstacles)."""
    for st in (["R23", "R24", "R25", "R26"], ["R28", "R29", "R30", "R31"], ["R33", "R34", "R35", "R36"],
               ["R39", "R40", "R41", "R42"], ["R45", "R46", "R47", "R48"]):
        chain(board, st)
    n = board.FindNet

    def P(r, k):
        return xy(pad(board, r, k))
    # HV+: J3.1 -> VBUS row HV end -> R0P row HV end
    track(board, [P("J3", 1), P("R33", 1)], n("HV_P"))
    track(board, [P("R33", 1), P("R23", 1)], n("HV_P"))
    # R0P_SW: U8 S1 -> R0P row low end;  R0N_SW: U9 S2 -> R0N row low end
    track(board, [P("U8", 11), P("R26", 2)], n("R0P_SW"))
    track(board, [P("U9", 9), P("R28", 1)], n("R0N_SW"))
    # E: U8 S2 <-> U9 S1, then along y = 28 to J4.1, with a spur up to the VE row HV end.
    s2, s1, je, ve = P("U8", 9), P("U9", 11), P("J4", 1), P("R39", 1)
    track(board, [s2, s1], n("E"))
    track(board, [(s2[0], je[1]), je], n("E"))
    track(board, [(94.0, je[1]), (94.0, ve[1]), ve], n("E"))
    # Escape stubs: the low end of each string sits 1.8 mm from an HV node of the same string, which
    # the autorouter (one 2.5 mm HV clearance for everything) cannot leave. A locked stub to a via
    # clear of the HV copper gives it a legal start point. GND_HV's via drops into the bottom pour.
    for ref, num, dx in (("R36", 2, -1.8), ("R42", 2, -1.8), ("R48", 2, -1.8), ("R31", 2, 1.8)):
        pd = pad(board, ref, num)
        x, y = xy(pd)
        track(board, [(x, y), (x + dx, y)], pd.GetNet(), 0.3)
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pt(x + dx, y)); v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetNet(pd.GetNet())
        v.SetLocked(True)
        board.Add(v)
    # U3 VINP (pin 3) -> C10, its 10 uF input cap: short and fixed (the 1206 C10 sits in the
    # autorouter's way otherwise).
    vx, vy = P("U3", 3)
    cx, cy = P("C10", 1)
    track(board, [(vx, vy), (vx - 1.5, vy), (cx, cy)], n("5V_LV"), 0.4)
    # KE: J4.2 -> VKE row HV end
    jk, vk = P("J4", 2), P("R45", 1)
    track(board, [jk, (94.0, jk[1]), (94.0, vk[1]), vk], n("KE"))


def strip_unused_pad(board, ref, num):
    """Remove the copper of an unloaded connector position (J3 pin 2): keeps HV+ / HV- 6 mm apart."""
    p = pad(board, ref, num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    p.SetSize(pcbnew.VECTOR2I(p.GetDrillSize().x, p.GetDrillSize().y))
    p.SetNetCode(0)


def hole_keepouts(board, radius=4.0, segs=32):
    layers = pcbnew.LSET()
    layers.AddLayer(pcbnew.F_Cu)
    layers.AddLayer(pcbnew.B_Cu)
    for fp in board.GetFootprints():
        if not fp.GetReference().startswith("H"):
            continue
        c = fp.GetPosition()
        z = pcbnew.ZONE(board)
        z.SetIsRuleArea(True)
        z.SetLayerSet(layers)
        z.SetDoNotAllowZoneFills(True); z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True)
        z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
        z.SetZoneName("keepout " + fp.GetReference())
        o = z.Outline()
        o.NewOutline()
        for i in range(segs):
            a = 2 * math.pi * i / segs
            o.Append(c.x + int(mm(radius) * math.cos(a)), c.y + int(mm(radius) * math.sin(a)))
        board.Add(z)


def barrier_keepout(board):
    """No copper at all in the 8 mm LV/HV gap, except the barrier packages' own pads."""
    layers = pcbnew.LSET()
    layers.AddLayer(pcbnew.F_Cu)
    layers.AddLayer(pcbnew.B_Cu)
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetLayerSet(layers)
    z.SetDoNotAllowZoneFills(True); z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    z.SetZoneName("isolation barrier")
    o = z.Outline()
    o.NewOutline()
    for x, y in ((LV_MAX_X, NOTCH[2]), (HV_MIN_X, NOTCH[2]), (HV_MIN_X, H), (LV_MAX_X, H)):
        o.Append(mm(x), mm(y))
    board.Add(z)


def place_refs(board):
    """Put each reference at the first clear spot (default, above, below, left, right of courtyard)."""
    def inflate(bb, d):
        bb = pcbnew.BOX2I(bb.GetPosition(), bb.GetSize())
        bb.Inflate(mm(d))
        return bb
    obstacles = []
    for fp in board.GetFootprints():
        for p in fp.Pads():
            obstacles.append(inflate(p.GetBoundingBox(), 0.2))
        for g in fp.GraphicalItems():
            if g.GetLayer() == pcbnew.F_SilkS:
                obstacles.append(inflate(g.GetBoundingBox(), 0.1))
    for d in board.GetDrawings():
        if d.GetLayer() == pcbnew.F_SilkS:
            obstacles.append(inflate(d.GetBoundingBox(), 0.1))
    edge = pcbnew.BOX2I(pt(0.5, 0.5), pcbnew.VECTOR2I(mm(W - 1), mm(H - 1)))
    for fp in sorted(board.GetFootprints(), key=lambda f: f.GetReference()):
        ref = fp.Reference()
        if not ref.IsVisible():
            continue
        ref.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8)))
        ref.SetTextThickness(mm(0.12))
        ref.SetTextAngleDegrees(0)
        cy = fp.GetCourtyard(pcbnew.F_CrtYd)
        bb = cy.BBox() if cy.OutlineCount() else fp.GetBoundingBox(False)
        c = bb.GetCenter()
        hw, hh = bb.GetWidth() // 2, bb.GetHeight() // 2
        cands = [ref.GetPosition()]
        for k in (0.6, 1.6):
            cands += [pcbnew.VECTOR2I(c.x, c.y - hh - mm(k)), pcbnew.VECTOR2I(c.x, c.y + hh + mm(k)),
                      pcbnew.VECTOR2I(c.x - hw - mm(k + 0.8), c.y), pcbnew.VECTOR2I(c.x + hw + mm(k + 0.8), c.y),
                      pcbnew.VECTOR2I(c.x - hw - mm(k), c.y - hh - mm(k)), pcbnew.VECTOR2I(c.x + hw + mm(k), c.y - hh - mm(k)),
                      pcbnew.VECTOR2I(c.x - hw - mm(k), c.y + hh + mm(k)), pcbnew.VECTOR2I(c.x + hw + mm(k), c.y + hh + mm(k))]
        chosen = None
        for p in cands:
            ref.SetPosition(p)
            tb = ref.GetBoundingBox()
            if edge.Contains(tb) and not any(tb.Intersects(o) for o in obstacles):
                chosen = p
                break
        if chosen is None:
            ref.SetPosition(cands[0])
            ref.SetVisible(False)        # no clear spot: reference stays on F.Fab only
            print("hid silkscreen reference", fp.GetReference())
            continue
        obstacles.append(inflate(ref.GetBoundingBox(), 0.1))


# Resistor strings, top (HV end) to bottom. Only NEIGHBOURING nodes of a string get the relaxed
# clearance; every other pair (e.g. VBUS_DIV vs HV_P, E vs R0N_3) keeps the full 2.5 mm, so DRC
# still checks the row ends that carry the whole pack voltage.
STRINGS = {
    "VBUS": ["HV_P", "VBUS_1", "VBUS_2", "VBUS_3", "VBUS_DIV"],
    "R0P": ["HV_P", "R0P_1", "R0P_2", "R0P_3", "R0P_SW"],
    "VE": ["E", "VE_1", "VE_2", "VE_3", "VE_DIV"],
    "VKE": ["KE", "VKE_1", "VKE_2", "VKE_3", "VKE_DIV"],
    "R0N": ["R0N_SW", "R0N_1", "R0N_2", "R0N_3", "GND_HV"],
}


def dru():
    out = ["(version 1)",
           "# Generated by gen_pcb.py. HV netclass = 2.5 mm to everything (IEC 60664-1, PD2,",
           "# material group III, 500 V row). Neighbouring nodes of one resistor string differ by at",
           "# most 1/4 of the pack voltage (112 V at 450 V): 0.5 mm there (125 V row: 0.4 mm basic).",
           "# Netclass and these rules apply on both copper layers, zone fills included."]
    for name, nodes in STRINGS.items():
        for a, b in zip(nodes, nodes[1:]):
            out.append(f'(rule "{name} {a}-{b}" (constraint clearance (min 0.5mm))')
            out.append(f"  (condition \"(A.NetName == '{a}' && B.NetName == '{b}') || "
                       f"(A.NetName == '{b}' && B.NetName == '{a}')\"))")
    out += ["# HV through-hole pins (J3 HV+, J4 E/KE) reach the bottom layer, where the GND_HV pour is:",
            "# all non-HV copper (pour, tracks, vias, pads) stays 1 mm further back than the 2.5 mm",
            "# netclass minimum - margin for the hand-soldered joints and flux residue on the side",
            "# facing the economizer. HV-to-HV pairs keep their own rules above.",
            "(rule \"HV THT pad margin\" (constraint clearance (min 3.5mm))",
            "  (condition \"(A.NetClass == 'HV' && A.Type == 'Pad' && A.Pad_Type == 'Through-hole' && B.NetClass != 'HV')"
            " || (B.NetClass == 'HV' && B.Type == 'Pad' && B.Pad_Type == 'Through-hole' && A.NetClass != 'HV')\"))"]
    out += ["# E and KE are the same chassis potential in operation (separate wires only to detect a",
            "# break); they sit on 3 mm connector pins. 1 mm functional spacing.",
            "(rule \"E-KE\" (constraint clearance (min 1.0mm))",
            "  (condition \"(A.NetName == 'E' && B.NetName == 'KE') || (A.NetName == 'KE' && B.NetName == 'E')\"))"]
    return "\n".join(out) + "\n"


DRU = dru()


def place():
    board = pcbnew.NewBoard(PCB)
    setup_rules(board)
    nets = {}
    for name in design.nets():
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni
    import kilib
    nc_nets = {}
    for p in design.parts:
        if not p["footprint"] or not p["pins"] and p["lib_id"] == "Mechanical:MountingHole":
            continue
        for num, (pname, etype, *_r) in kilib.pins(p["lib_id"]).items():
            if not p["pins"].get(num):
                name = f"unconnected-({p['ref']}-{pname.replace('/', '{slash}')}-Pad{num})"
                ni = pcbnew.NETINFO_ITEM(board, name)
                board.Add(ni)
                nc_nets[(p["ref"], num)] = ni
    pos = placements()
    for p in design.parts:
        if not p["footprint"]:
            continue
        fp = load_fp(p["footprint"])
        fp.SetReference(p["ref"])
        fp.SetValue(p["value"])
        fp.SetPath(pcbnew.KIID_PATH("/" + uid("sym", p["ref"])))
        x, y, rot = pos[p["ref"]]
        board.Add(fp)
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        for pd in fp.Pads():
            net = p["pins"].get(pd.GetNumber())
            if net:
                pd.SetNet(nets[net])
            elif (p["ref"], pd.GetNumber()) in nc_nets:
                pd.SetNet(nc_nets[(p["ref"], pd.GetNumber())])
        for key, val in (("MPN", p["mpn"]), ("LCSC", p["lcsc"]), ("Notes", p["note"])):
            if val:
                fp.SetField(key, val)
                fp.GetField(key).SetVisible(False)
        if p["dnp"]:
            fp.SetDNP(True)
        if p["ref"].startswith("H"):
            fp.SetExcludedFromBOM(True)
            fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
    outline(board)
    for ref in ("U3", "U4", "U5"):
        fp = board.FindFootprintByReference(ref)
        c = fp.GetPosition()
        h = 7.0 if ref != "U5" else 3.0
        slot(board, pcbnew.ToMM(c.x), pcbnew.ToMM(c.y) - h / 2, pcbnew.ToMM(c.y) + h / 2)
    strip_unused_pad(board, "J3", 2)
    hole_keepouts(board)
    barrier_keepout(board)
    hv_routes(board)
    # Silkscreen
    silk(board, "LV", 16.0, 63.5, 1.5)
    silk(board, "HV  DANGER  450 V", 55.0, 63.5, 1.5)
    silk(board, "HV+", 93.0, 10.0, 0.9)
    silk(board, "HV-", 93.0, 20.0, 0.9)
    silk(board, "E", 98.0, 25.8, 0.9)
    silk(board, "KE", 98.0, 33.3, 0.9)
    for x, y in ((72.5, 8.8), (72.5, 57.2), (W - 10.0, 3.5), (W - 3.5, 57.2)):
        silk(board, "NYLON", x, y, 0.8)
    silk(board, "IMD board rev A - DIY, uncertified", 50, 30, 1.0, pcbnew.B_SilkS)
    place_refs(board)
    u3 = board.FindFootprintByReference("U3").Reference()
    u3.SetPosition(pt(BARRIER_X, 20.8)); u3.SetVisible(True)
    board.BuildConnectivity()
    pcbnew.SaveBoard(PLACED, board)
    pcbnew.SaveBoard(PCB, board)
    for f in (PCB, PLACED):
        with open(f.replace(".kicad_pcb", ".kicad_dru"), "w") as fh:
            fh.write(DRU)
    ok = pcbnew.ExportSpecctraDSN(board, os.path.join(HERE, "imd.dsn"))
    print("placed", len(pos), "parts; DSN export", ok)


def pour(board, net, layer, poly, clearance=0.3):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetLocalClearance(mm(clearance))
    z.SetMinThickness(mm(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)   # spokes on THT pads only; SMD pads solid
    z.SetThermalReliefGap(mm(0.3))
    z.SetThermalReliefSpokeWidth(mm(0.5))
    o = z.Outline()
    o.NewOutline()
    for x, y in poly:
        o.Append(mm(x), mm(y))
    board.Add(z)
    return z


def stitch(board, net, zones, x0, x1, pitch=3.0, r_check=0.36):
    """GND vias on a grid wherever both layers' fills of this net contain the via (see economizer)."""
    for z in zones:
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_NEVER)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    fills = [z.GetFilledPolysList(z.GetLayer()) for z in zones]
    keep_out = []
    for fp in board.GetFootprints():
        for p in fp.Pads():
            bb = p.GetBoundingBox()
            keep_out.append((bb.GetCenter().x, bb.GetCenter().y, max(bb.GetWidth(), bb.GetHeight()) / 2 + mm(0.9)))
    for t in board.GetTracks():
        if t.GetClass() == "PCB_VIA":
            keep_out.append((t.GetPosition().x, t.GetPosition().y, mm(1.2)))
    ring = [(0, 0)] + [(r_check * c, r_check * s_) for c, s_ in
                       ((1, 0), (-1, 0), (0, 1), (0, -1), (0.7071, 0.7071), (-0.7071, 0.7071),
                        (0.7071, -0.7071), (-0.7071, -0.7071))]
    added = 0
    y = 1.5
    while y < H - 1.0:
        x = x0
        while x < x1:
            cx, cy = mm(x), mm(y)
            ok = all(all(f.Contains(pcbnew.VECTOR2I(cx + mm(dx), cy + mm(dy))) for dx, dy in ring) for f in fills)
            if ok and all((cx - kx) ** 2 + (cy - ky) ** 2 > kr ** 2 for kx, ky, kr in keep_out):
                v = pcbnew.PCB_VIA(board)
                v.SetPosition(pcbnew.VECTOR2I(cx, cy))
                v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetNet(net)
                board.Add(v)
                keep_out.append((cx, cy, mm(1.2)))
                added += 1
            x += pitch
        y += pitch
    for z in zones:
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    return added


def finish():
    shutil.copyfile(os.path.join(HERE, "imd.kicad_pro"), PLACED.replace(".kicad_pcb", ".kicad_pro"))
    board = pcbnew.LoadBoard(PLACED)
    ses = os.path.join(HERE, "imd.ses")
    print("SES import", pcbnew.ImportSpecctraSES(board, ses))
    for t in board.GetTracks():
        if t.GetClass() == "PCB_TRACK" and t.GetWidth() < mm(0.15):
            t.SetWidth(mm(0.15))
        if t.GetClass() == "PCB_VIA":
            w = t.GetWidth(pcbnew.F_Cu)
            if (w - t.GetDrillValue()) / 2 < mm(0.1):
                t.SetDrill(max(mm(0.3), w - mm(0.3)))
    e = 0.4
    x0, x1, d = NOTCH
    lv = [(e, e), (x0 - e, e), (x0 - e, d + e), (LV_MAX_X, d + e), (LV_MAX_X, H - e), (e, H - e)]
    hv_top = [(HV_MIN_X, d + e), (x1 + e, d + e), (x1 + e, e), (59.0, e), (59.0, 43.0), (W - e, 43.0),
              (W - e, H - e), (HV_MIN_X, H - e)]
    hv_bot = [(HV_MIN_X, d + e), (x1 + e, d + e), (x1 + e, e), (W - e, e), (W - e, H - e), (HV_MIN_X, H - e)]
    gnd, gndhv = board.FindNet("GND"), board.FindNet("GND_HV")
    # Solid pour connection where thermal spokes are starved by neighbouring tracks.
    solid = {"J1": ("2", "4", "8"), "J2": ("2",)}
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNumber() in solid.get(fp.GetReference(), ()):
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
    lvz = [pour(board, gnd, pcbnew.B_Cu, lv), pour(board, gnd, pcbnew.F_Cu, lv)]
    hvz = [pour(board, gndhv, pcbnew.B_Cu, hv_bot), pour(board, gndhv, pcbnew.F_Cu, hv_top)]
    n1 = stitch(board, gnd, lvz, 1.5, LV_MAX_X - 1.0)
    n2 = stitch(board, gndhv, hvz, HV_MIN_X + 1.0, 59.0)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    removed = prune_vias(board, lvz + hvz)
    # Router GND_HV tracks inside the 3.5 mm margin around HV through-hole pins are redundant (the
    # bottom GND_HV pour connects both ends); drop them so the margin rule holds. Deleting board
    # items confuses SWIG for later object creation, so only the refill and save follow.
    cut = clear_hv_margin(board, "GND_HV", 3.5)
    if cut:
        print("removed", cut, "GND_HV track segment(s) inside the HV pin margin")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    print("stitching vias", n1, n2, "pruned", removed, "tracks", len(board.GetTracks()))


def clear_hv_margin(board, netname, margin):
    """Delete unlocked tracks of `netname` that come within `margin` mm of an HV through-hole pad
    (edge to edge, on any layer). Whole connected runs are not traced: only offending segments go,
    which is enough when the net is a pour net that reconnects through the fill."""
    hv = set(HV_NETS)
    pads = [p for fp in board.GetFootprints() for p in fp.Pads()
            if p.GetNetname() in hv and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    removed = 0
    for t in list(board.GetTracks()):
        if t.GetClass() != "PCB_TRACK" or t.GetNetname() != netname or t.IsLocked():
            continue
        seg = pcbnew.SEG(t.GetStart(), t.GetEnd())
        for p in pads:
            r = max(p.GetSize(pcbnew.F_Cu).x, p.GetSize(pcbnew.F_Cu).y) / 2
            if pcbnew.ToMM(seg.Distance(p.GetPosition()) - r - t.GetWidth() / 2) < margin:
                board.Remove(t)
                removed += 1
                break
    return removed


def prune_vias(board, zones):
    """Remove vias that end up connected on one layer only (stitching vias whose pour piece was
    removed as an island, escape vias the router reached on the top layer)."""
    removed = 0
    for v in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
        pos, net = v.GetPosition(), v.GetNetCode()
        layers_hit = set()
        for t in board.GetTracks():
            if t.GetClass() == "PCB_TRACK" and t.GetNetCode() == net and pos in (t.GetStart(), t.GetEnd()):
                layers_hit.add(t.GetLayer())
        for z in zones:
            if z.GetNetCode() == net and z.GetFilledPolysList(z.GetLayer()).Contains(pos):
                layers_hit.add(z.GetLayer())
        for fp in board.GetFootprints():
            for p in fp.Pads():
                if p.GetNetCode() == net and p.HitTest(pos):
                    layers_hit.update((pcbnew.F_Cu, pcbnew.B_Cu) if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH
                                      else (pcbnew.F_Cu,))
        if len(layers_hit) < 2:
            board.Remove(v)
            removed += 1
    return removed


if __name__ == "__main__":
    {"place": place, "finish": finish}[sys.argv[1]]()
