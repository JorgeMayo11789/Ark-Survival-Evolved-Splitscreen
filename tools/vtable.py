"""Print the entries of a vtable (by symbol name substring) with resolved function names.

Usage: python vtable.py <game.exe> <symbols.tsv> <vftable-name-substring> [count]
Reads only.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_sections, load_symbols, rva_to_file  # noqa: E402


def main():
    exe_path, sym_path, query, *rest = sys.argv[1:]
    count = int(rest[0]) if rest else 16
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    with open(exe_path, "rb") as f:
        head = f.read(4096)
        (pe_off,) = struct.unpack_from("<I", head, 0x3C)
        (image_base,) = struct.unpack_from("<Q", head, pe_off + 24 + 24)
        for rva, name in sorted(by_rva.items()):
            if query not in name or "vftable" not in name:
                continue
            print(f"### {name}  (RVA {rva:08X})")
            f.seek(rva_to_file(sections, rva))
            for i in range(count):
                (va,) = struct.unpack("<Q", f.read(8))
                target = va - image_base
                print(f"  +0x{i * 8:02X}  {target:08X}  {by_rva.get(target, '?')}")
            print()


if __name__ == "__main__":
    main()
