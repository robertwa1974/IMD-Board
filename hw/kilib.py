"""Load symbols from KiCad stock libraries and the project library; list pins."""
import copy
import os

from sexpr import parse, find, find_all, Sym

KICAD_SHARE = os.environ.get("KICAD_SHARE", r"C:\Program Files\KiCad\10.0\share\kicad")
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_LIB = os.path.join(HERE, "imd.kicad_sym")

_cache = {}


def _lib(libname):
    if libname not in _cache:
        path = PROJECT_LIB if libname == "imd" else os.path.join(KICAD_SHARE, "symbols", libname + ".kicad_sym")
        with open(path, encoding="utf-8") as f:
            tree = parse(f.read())
        _cache[libname] = {s[1]: s for s in find_all(tree, "symbol")}
    return _cache[libname]


def get_symbol(lib_id):
    """Return a flattened symbol definition (extends resolved) named lib_id."""
    libname, name = lib_id.split(":")
    syms = _lib(libname)
    sym = copy.deepcopy(syms[name])
    ext = find(sym, "extends")
    if ext:
        parent = copy.deepcopy(syms[ext[1]])
        # Child keeps its own properties; graphics/pins come from the parent units.
        child_props = {p[1]: p for p in find_all(sym, "property")}
        out = [Sym("symbol"), name]
        for item in parent[2:]:
            if isinstance(item, list) and item[0] == "property" and item[1] in child_props:
                out.append(child_props.pop(item[1]))
            elif isinstance(item, list) and item[0] == "symbol":
                unit = copy.deepcopy(item)
                unit[1] = unit[1].replace(ext[1], name, 1)
                out.append(unit)
            else:
                out.append(item)
        out.extend(child_props.values())
        sym = out
    sym[1] = lib_id
    return sym


def pins(lib_id):
    """{number: (name, electrical_type, x, y, angle, unit)} in symbol coordinates (y up)."""
    sym = get_symbol(lib_id)
    out = {}
    for unit in find_all(sym, "symbol"):
        parts = unit[1].rsplit("_", 2)
        unit_no = int(parts[-2]) if len(parts) >= 3 and parts[-2].isdigit() else 1
        for p in find_all(unit, "pin"):
            at = find(p, "at")
            num = find(p, "number")[1]
            name = find(p, "name")[1]
            out[num] = (name, str(p[1]), float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0, unit_no)
    return out


if __name__ == "__main__":
    import sys
    for lid in sys.argv[1:]:
        print(lid)
        for num, v in sorted(pins(lid).items(), key=lambda kv: (len(kv[0]), kv[0])):
            print(f"  {num:>4} {v[0]:<22} {v[1]:<14} ({v[2]:g},{v[3]:g}) a{v[4]:g} u{v[5]}")
