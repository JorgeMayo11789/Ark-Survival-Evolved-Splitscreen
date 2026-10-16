"""Find instructions in .text that write a byte to a named global via RIP-relative addressing.

Usage: python writes.py <game.exe> <symbols.tsv> <global-name>

Covers `mov byte ptr [rip+disp], imm8` (C6 05) and `mov byte ptr [rip+disp], r8` (88 /r, mod=00 rm=101,
optionally REX-prefixed). Reads only.
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_sections, load_symbols  # noqa: E402


def main():
    exe_path, sym_path, name = sys.argv[1:4]
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    starts = sorted(by_rva)
    targets = [rva for rva, n in by_rva.items() if n.endswith(name)]
    if not targets:
        sys.exit(f"no symbol named {name}")
    target = targets[0]

    _n, vaddr, _size, raw_ptr, raw_size = next(s for s in sections if s[0] == ".text")
    with open(exe_path, "rb") as f:
        f.seek(raw_ptr)
        code = f.read(raw_size)

    def owner(rva):
        i = bisect.bisect_right(starts, rva) - 1
        return by_rva[starts[i]] if i >= 0 else "?"

    hits = []
    # mov byte ptr [rip+disp32], imm8  ->  C6 05 disp32 imm8  (end = disp + 5)
    pos = code.find(b"\xC6\x05")
    while pos != -1:
        (disp,) = struct.unpack_from("<i", code, pos + 2)
        if vaddr + pos + 7 + disp == target:
            hits.append((vaddr + pos, f"mov byte [GLOBAL], {code[pos + 6]}"))
        pos = code.find(b"\xC6\x05", pos + 1)
    # mov byte ptr [rip+disp32], r8  ->  [REX] 88 modrm(00 reg 101) disp32
    pos = code.find(b"\x88")
    while pos != -1 and pos + 6 <= len(code):
        modrm = code[pos + 1]
        if modrm & 0xC7 == 0x05:
            (disp,) = struct.unpack_from("<i", code, pos + 2)
            if vaddr + pos + 6 + disp == target:
                start = pos - 1 if pos and 0x40 <= code[pos - 1] <= 0x4F else pos
                hits.append((vaddr + start, f"mov byte [GLOBAL], reg{(modrm >> 3) & 7}"))
        pos = code.find(b"\x88", pos + 1)

    print(f"### writes to {by_rva[target]} (RVA {target:08X})")
    for rva, what in sorted(hits):
        print(f"  {rva:08X}  {what:<28} in {owner(rva)}")
    if not hits:
        print("  (none found)")


if __name__ == "__main__":
    main()
