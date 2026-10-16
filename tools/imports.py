"""List the DLLs (and optionally functions) a PE imports.

Usage: python imports.py <file.exe> [dll-name-to-expand ...]
Reads only.
"""
import struct
import sys


def main():
    path, *expand = sys.argv[1:]
    expand = {e.lower() for e in expand}
    with open(path, "rb") as f:
        data = f.read()
    (pe_off,) = struct.unpack_from("<I", data, 0x3C)
    (n_sections,) = struct.unpack_from("<H", data, pe_off + 6)
    (opt_size,) = struct.unpack_from("<H", data, pe_off + 20)
    opt = pe_off + 24
    (import_rva, _import_size) = struct.unpack_from("<II", data, opt + 112 + 8)  # data dir 1 (PE32+)
    (delay_rva, _delay_size) = struct.unpack_from("<II", data, opt + 112 + 13 * 8)
    sections = []
    for i in range(n_sections):
        base = opt + opt_size + 40 * i
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((vaddr, max(vsize, raw_size), raw_ptr))

    def off(rva):
        for vaddr, size, raw in sections:
            if vaddr <= rva < vaddr + size:
                return raw + rva - vaddr
        raise ValueError(hex(rva))

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\x00", o)].decode("ascii", "replace")

    def thunks(rva):
        names = []
        o = off(rva)
        while True:
            (entry,) = struct.unpack_from("<Q", data, o)
            if entry == 0:
                return names
            names.append(f"#{entry & 0xFFFF}" if entry >> 63 else cstr((entry & 0x7FFFFFFF) + 2))
            o += 8

    print("== imports ==")
    o = off(import_rva)
    while True:
        orig_first, _ts, _fwd, name_rva, first = struct.unpack_from("<IIIII", data, o)
        if name_rva == 0:
            break
        name = cstr(name_rva)
        print(name)
        if name.lower() in expand:
            for fn in thunks(orig_first or first):
                print("    ", fn)
        o += 20

    if delay_rva:
        print("== delay-load imports ==")
        o = off(delay_rva)
        while True:
            _attr, name_rva, _hmod, _iat, int_rva = struct.unpack_from("<IIIII", data, o)[:5]
            if name_rva == 0:
                break
            name = cstr(name_rva)
            print(name)
            if name.lower() in expand:
                for fn in thunks(int_rva):
                    print("    ", fn)
            o += 32


if __name__ == "__main__":
    main()
