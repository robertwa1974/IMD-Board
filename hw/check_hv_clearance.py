"""Measure the real copper gap around HV through-hole pads and HV vias on BOTH layers.

DRC already enforces the HV netclass clearance on every copper layer (zone fills included); this
script is an independent check that prints the numbers. Run with KiCad's python after `finish`:

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" check_hv_clearance.py

For every plated pad / via whose net is in the HV netclass it reports, per copper layer, the
smallest edge-to-edge gap to copper of any other net: zone fills (the GND_HV pour), tracks, vias
and pads. Exact KiCad outlines are used; unplated holes (connector pegs, J3's stripped middle
position) carry no copper and are ignored. The relaxed per-neighbour string rules do not apply to
these pads: they need the 3.5 mm "HV THT pad margin", except the E-KE pair (own 1.0 mm rule).
"""
import math
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_pcb import HV_NETS  # noqa: E402

MIN_GAP = 3.5   # "HV THT pad margin" rule in imd.kicad_dru (netclass minimum is 2.5)
PAIR_MIN = {frozenset(("E", "KE")): 1.0}   # pairs with their own rule in imd.kicad_dru
LAYERS = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}
MAX_ERR = pcbnew.FromMM(0.005)


def outline(item, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    item.TransformShapeToPolygon(ps, layer, 0, MAX_ERR, pcbnew.ERROR_INSIDE)
    return ps


def vertices(ps):
    for i in range(ps.OutlineCount()):
        o = ps.Outline(i)
        for k in range(o.PointCount()):
            yield o.CPoint(k)


def poly_gap(a, b):
    """Edge-to-edge distance between two polygon sets in mm (0 if they touch or overlap)."""
    best = math.inf
    for v in vertices(a):
        best = min(best, b.SquaredDistance(v))
    for v in vertices(b):
        best = min(best, a.SquaredDistance(v))
    return pcbnew.ToMM(int(math.sqrt(best)))


def copper(item):
    return not (isinstance(item, pcbnew.PAD) and item.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH)


def main():
    board = pcbnew.LoadBoard(os.path.join(HERE, "imd.kicad_pcb"))
    hv = set(HV_NETS)
    pads = [p for fp in board.GetFootprints() for p in fp.Pads() if copper(p)]
    hv_items = [(f"{p.GetParentFootprint().GetReference()}.{p.GetNumber()}", p) for p in pads
                if p.GetNetname() in hv and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    hv_vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() in hv]
    hv_items += [(f"via@({pcbnew.ToMM(v.GetPosition().x):.1f},{pcbnew.ToMM(v.GetPosition().y):.1f})", v)
                 for v in hv_vias]
    print(f"HV-class vias on the board: {len(hv_vias)}")

    worst_all = math.inf
    pair_worst = {k: math.inf for k in PAIR_MIN}
    for label, item in hv_items:
        net = item.GetNetname()
        row = []
        for lname, layer in LAYERS.items():
            mine = outline(item, layer)
            worst, who = math.inf, ""
            others = [(z.GetFilledPolysList(layer), z.GetNetname(), "pour " + z.GetNetname())
                      for z in board.Zones()
                      if z.IsOnLayer(layer) and not z.GetIsRuleArea() and z.GetNetname() != net]
            for t in board.GetTracks():
                if t.GetNetname() != net and t.IsOnLayer(layer):
                    others.append((outline(t, layer), t.GetNetname(), t.GetClass().lower() + " " + t.GetNetname()))
            for p in pads:
                if p.GetNetname() != net and p.IsOnLayer(layer):
                    others.append((outline(p, layer), p.GetNetname(),
                                   f"{p.GetParentFootprint().GetReference()}.{p.GetNumber()} {p.GetNetname()}"))
            c = item.GetPosition()
            for ps, onet, desc in others:
                bb = ps.BBox()
                # cheap reject: anything whose bounding box is > 8 mm away cannot be the nearest
                if bb.Distance(c) > pcbnew.FromMM(8):
                    continue
                d = poly_gap(mine, ps)
                k = frozenset((net, onet))
                if k in PAIR_MIN:
                    pair_worst[k] = min(pair_worst[k], d)
                elif d < worst:
                    worst, who = d, desc
            row.append(f"{lname} {worst:5.2f} mm ({who})")
            worst_all = min(worst_all, worst)
        print(f"{label:<8} {net:<6} " + "   ".join(row))
    status = "OK" if worst_all >= MIN_GAP - 0.01 else "TOO CLOSE"
    for k, d in pair_worst.items():
        ok = d >= PAIR_MIN[k] - 0.01
        print(f"{'-'.join(sorted(k))} pair (own rule {PAIR_MIN[k]} mm): {d:.2f} mm -> {'OK' if ok else 'TOO CLOSE'}")
        if not ok:
            status = "TOO CLOSE"
    print(f"smallest gap from an HV through-hole pad / via: {worst_all:.2f} mm (need {MIN_GAP}) -> {status}")
    return 0 if status == "OK" else 1


if __name__ == "__main__":
    sys.exit(main())
