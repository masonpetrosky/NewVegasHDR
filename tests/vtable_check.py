"""Checks the vtable slots in src/vtable.h against the method order in the Windows SDK's d3d9.h.

usage: python -I vtable_check.py <path to d3d9.h> <path to src/vtable.h>
"""
import re
import sys


def methods(header, interface):
    # The C++ declaration: DECLARE_INTERFACE_(IDirect3DDevice9, IUnknown) { ... STDMETHOD(Name)(...) ... };
    m = re.search(r"DECLARE_INTERFACE_\(\s*%s\s*,\s*(\w+)\s*\)\s*\{(.*?)\n\};" % interface, header, re.S)
    if not m:
        sys.exit(f"{interface} not found")
    names = re.findall(r"STDMETHOD_?\(\s*(?:\w+\s*,\s*)?(\w+)\s*\)", m.group(2))
    return names


def main():
    header = open(sys.argv[1], encoding="latin-1").read()
    ours = open(sys.argv[2]).read()
    tables = {
        "IDirect3D9": methods(header, "IDirect3D9"),
        "IDirect3DDevice9": methods(header, "IDirect3DDevice9"),
        "IDirect3DSwapChain9": methods(header, "IDirect3DSwapChain9"),
    }
    section, bad, checked = None, 0, 0
    for line in ours.splitlines():
        h = re.match(r"// (IDirect3D\w+)$", line.strip())
        if h:
            section = h.group(1)
            continue
        d = re.match(r"constexpr int k(\w+) = (\d+);", line.strip())
        if d and section:
            name, index = d.group(1), int(d.group(2))
            if section == "IDirect3DSwapChain9" and name.startswith("SwapChain"):
                name = name[len("SwapChain"):]
            actual = tables[section][index] if index < len(tables[section]) else None
            checked += 1
            if actual != name:
                bad += 1
                print(f"{section} slot {index}: vtable.h says {name}, d3d9.h has {actual}")
    print(f"{checked} slots checked, {bad} wrong")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
