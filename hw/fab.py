"""Fabrication outputs for JLCPCB: Gerbers + drill (zipped), BOM and CPL CSVs, 3D renders.

usage: python fab.py      (writes hw/fab/)
"""
import csv
import os
import subprocess
import zipfile
from collections import OrderedDict

import design

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fab")
GERB = os.path.join(OUT, "gerbers")
HAND = {"J1", "J2", "J3", "J4", "J5"}   # through-hole: hand-soldered, kept out of the CPL
CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"
PCB = os.path.join(HERE, "imd.kicad_pcb")


def run(*args):
    subprocess.run([CLI, *args], check=True, capture_output=True)


def gerbers():
    os.makedirs(GERB, exist_ok=True)
    for f in os.listdir(GERB):
        os.remove(os.path.join(GERB, f))
    layers = "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts"
    run("pcb", "export", "gerbers", "-l", layers, "--subtract-soldermask", "--no-x2", "--use-drill-file-origin",
        "-o", GERB + os.sep, PCB)
    run("pcb", "export", "drill", "--format", "excellon", "--drill-origin", "plot", "--excellon-units", "mm",
        "--excellon-separate-th", "--generate-map", "--map-format", "gerberx2", "-o", GERB + os.sep, PCB)
    z = os.path.join(OUT, "imd_gerbers_jlcpcb.zip")
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(os.listdir(GERB)):
            zf.write(os.path.join(GERB, f), f)
    return z


def bom():
    """JLCPCB BOM: Comment, Designator, Footprint, LCSC Part #. Grouped by value+footprint+LCSC."""
    groups = OrderedDict()
    for p in design.parts:
        if not p["footprint"] or p["ref"].startswith(("#", "H")):
            continue
        fp = p["footprint"].split(":")[1]
        key = (p["value"], fp, p["lcsc"], p["mpn"])
        groups.setdefault(key, []).append(p)
    path = os.path.join(OUT, "imd_bom_jlcpcb.csv")
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "MPN", "Qty", "Notes"])
        for (val, fp, lcsc, mpn), ps in groups.items():
            refs = sorted((p["ref"] for p in ps), key=lambda r: (r.rstrip("0123456789"), int(r.lstrip("ABCDEFGHIJKLMNOPQRSTUVWXYZ#"))))
            uniq = {p["note"] for p in ps if p["note"]}
            if len(uniq) == 1 and all(p["note"] for p in ps):
                notes = uniq.pop()
            else:
                notes = "; ".join(f"{p['ref']}: {p['note']}" for p in ps if p["note"])
            if set(refs) & HAND:
                notes = ("HAND-SOLDER (not in CPL). " + notes).strip()
            w.writerow([val, ",".join(refs), fp, lcsc, mpn, len(ps), notes])
    return path


def cpl():
    """JLCPCB CPL: Designator, Mid X, Mid Y, Layer, Rotation (from KiCad's position file)."""
    tmp = os.path.join(OUT, "pos_raw.csv")
    run("pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both", "--use-drill-file-origin",
        "--exclude-dnp", "-o", tmp, PCB)
    path = os.path.join(OUT, "imd_cpl_jlcpcb.csv")
    with open(tmp, encoding="utf-8") as fi, open(path, "w", newline="", encoding="utf-8") as fo:
        r = csv.DictReader(fi)
        w = csv.writer(fo)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for row in r:
            ref = row["Ref"]
            if ref in HAND or ref.startswith("H"):
                continue
            w.writerow([ref, row["PosX"] + "mm", row["PosY"] + "mm",
                        "Top" if row["Side"].lower() == "top" else "Bottom", row["Rot"]])
    os.remove(tmp)
    return path


def renders():
    paths = []
    for side in ("top", "bottom"):
        p = os.path.join(OUT, f"imd_3d_{side}.png")
        try:
            run("pcb", "render", "--side", side, "--quality", "high", "--width", "1600", "--height", "1300",
                "--background", "opaque", "-o", p, PCB)
            paths.append(p)
        except subprocess.CalledProcessError as e:
            print("render failed:", e.stderr.decode(errors="replace")[:300])
    return paths


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for f in (gerbers(), bom(), cpl(), *renders()):
        print("wrote", os.path.relpath(f, HERE))
