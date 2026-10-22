"""List or extract the shaders in Fallout New Vegas shader packages (Data\\Shaders\\shaderpackage*.sdp).

usage: python -I sdp.py list <package.sdp> [name regex]
       python -I sdp.py extract <package.sdp> <out dir> [name regex]
       python -I sdp.py diff <package A.sdp> <package B.sdp>

A package is a header (u32 version = 100, u32 shader count, u32 size of the rest) followed by one entry per shader:
char name[256] (zero-terminated), u32 size, then the D3D9 shader bytecode with its constant table.
"""
import hashlib
import os
import re
import struct
import sys


def read_package(path):
    data = open(path, "rb").read()
    version, count, size = struct.unpack_from("<III", data, 0)
    if version != 100 or size != len(data) - 12:
        sys.exit(f"{path}: not a shader package (version {version}, size {size} vs {len(data) - 12})")
    shaders, i = {}, 12
    for _ in range(count):
        name = data[i:i + 256].split(b"\0", 1)[0].decode("ascii")
        (n,) = struct.unpack_from("<I", data, i + 256)
        shaders[name] = data[i + 260:i + 260 + n]
        i += 260 + n
    return shaders


def shader_model(code):
    (token,) = struct.unpack_from("<I", code, 0)
    kind = {0xFFFF: "ps", 0xFFFE: "vs"}.get(token >> 16, "??")
    return f"{kind}_{(token >> 8) & 0xFF}_{token & 0xFF}"


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "list" and len(sys.argv) >= 3:
        pattern = re.compile(sys.argv[3] if len(sys.argv) > 3 else ".", re.I)
        for name, code in read_package(sys.argv[2]).items():
            if pattern.search(name):
                print(f"{name:32} {shader_model(code):7} {len(code):6}  {hashlib.sha1(code).hexdigest()[:12]}")
    elif cmd == "extract" and len(sys.argv) >= 4:
        pattern = re.compile(sys.argv[4] if len(sys.argv) > 4 else ".", re.I)
        os.makedirs(sys.argv[3], exist_ok=True)
        n = 0
        for name, code in read_package(sys.argv[2]).items():
            if pattern.search(name):
                open(os.path.join(sys.argv[3], name), "wb").write(code)
                n += 1
        print(f"{n} shaders written to {sys.argv[3]}")
    elif cmd == "diff" and len(sys.argv) == 4:
        a, b = read_package(sys.argv[2]), read_package(sys.argv[3])
        only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
        changed = sorted(n for n in set(a) & set(b) if a[n] != b[n])
        print(f"{len(a)} vs {len(b)} shaders; {len(changed)} differ, {len(only_a)} only in A, {len(only_b)} only in B")
        for n in changed:
            print(f"  differs: {n}")
        for n in only_a:
            print(f"  only in A: {n}")
        for n in only_b:
            print(f"  only in B: {n}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
