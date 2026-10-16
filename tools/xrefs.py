"""Find direct callers of named functions (E8 rel32 calls / E9 jumps) in .text.

Usage: python xrefs.py <game.exe> <symbols.tsv> <name-substring> [...]

Reports each call site and the function that contains it. Indirect (virtual)
calls are not found. Reads only.
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_sections, load_symbols  # noqa: E402


def main():
    exe_path, sym_path, *queries = sys.argv[1:]
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    starts = sorted(by_rva)

    text = next(s for s in sections if s[0] == ".text")
    _name, vaddr, _size, raw_ptr, raw_size = text
    with open(exe_path, "rb") as f:
        f.seek(raw_ptr)
        code = f.read(raw_size)

    targets = {}
    for query in queries:
        for rva, name in by_rva.items():
            if query in name and "`" not in name:
                targets[rva] = name

    hits = {rva: [] for rva in targets}
    for op in (0xE8, 0xE9):
        pos = code.find(bytes([op]))
        while pos != -1 and pos + 5 <= len(code):
            (rel,) = struct.unpack_from("<i", code, pos + 1)
            dest = vaddr + pos + 5 + rel
            if dest in hits:
                site = vaddr + pos
                i = bisect.bisect_right(starts, site) - 1
                caller = by_rva[starts[i]] if i >= 0 else "?"
                hits[dest].append((site, "call" if op == 0xE8 else "jmp", caller))
            pos = code.find(bytes([op]), pos + 1)

    for rva, name in sorted(targets.items()):
        print(f"### {name}  (RVA {rva:08X})")
        if not hits[rva]:
            print("  (no direct callers found)")
        for site, kind, caller in hits[rva]:
            print(f"  {site:08X} {kind:<4} from {caller}")
        print()


if __name__ == "__main__":
    main()
