"""Compare KiCad's exported netlist (imd.net) with design.py."""
import sys
import design
from sexpr import parse, find, find_all

tree = parse(open("imd.net", encoding="utf-8").read())
kicad = {}
for net in find_all(find(tree, "nets"), "net"):
    name = find(net, "name")[1].lstrip("/")
    conns = {(find(n, "ref")[1], find(n, "pin")[1]) for n in find_all(net, "node")}
    if name.startswith("unconnected-") or name.startswith("Net-("):
        continue
    kicad[name] = conns
want = {n: {x for x in c if not x[0].startswith("#")} for n, c in design.nets().items()}
bad = 0
for n in sorted(set(want) | set(kicad)):
    w, k = want.get(n, set()), kicad.get(n, set())
    if w != k:
        bad += 1
        print(f"MISMATCH {n}: missing {sorted(w - k)} extra {sorted(k - w)}")
print(f"{len(want)} design nets, {len(kicad)} schematic nets, {bad} mismatches")
sys.exit(1 if bad else 0)
