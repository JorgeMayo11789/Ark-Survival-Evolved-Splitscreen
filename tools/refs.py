"""Find every instruction in .text that references a named global through RIP-relative addressing.

Usage: python refs.py <game.exe> <symbols.tsv> <global-name | 0xRVA | "wstr:Text">

`0xRVA` searches for an address directly; `wstr:Text` finds the UTF-16 string literal(s) "Text" in the
read-only data and searches for references to each.

Brute-force: for every offset, treat 4 bytes as a disp32 and check whether it would land on the target
for an instruction ending 0, 1, 2 or 4 bytes later; each candidate is then confirmed with capstone.
Slow (tens of seconds). Reads only.
"""
import bisect
import os
import sys
from array import array

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_sections, load_symbols  # noqa: E402


def find_wstr(exe_path, sections, text):
    needle = text.encode("utf-16-le") + b"\x00\x00"
    _n, vaddr, _size, raw_ptr, raw_size = next(s for s in sections if s[0] == ".rdata")
    with open(exe_path, "rb") as f:
        f.seek(raw_ptr)
        data = f.read(raw_size)
    hits = []
    pos = data.find(needle)
    while pos != -1:
        # Whole literal only: the preceding UTF-16 unit must be a terminator (or alignment padding).
        if pos % 2 == 0 and (pos < 2 or data[pos - 2:pos] == b"\x00\x00"):
            hits.append(vaddr + pos)
        pos = data.find(needle, pos + 1)
    return hits


def main():
    exe_path, sym_path, name = sys.argv[1:4]
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    starts = sorted(by_rva)
    if name.startswith("wstr:"):
        for target in find_wstr(exe_path, sections, name[5:]):
            scan(exe_path, sections, by_rva, starts, target, f'L"{name[5:]}"')
        return
    if name.startswith("0x"):
        target = int(name, 16)
        label = by_rva.get(target, name)
    else:
        target = next(rva for rva, n in by_rva.items() if n.endswith(name))
        label = by_rva[target]
    scan(exe_path, sections, by_rva, starts, target, label)


def scan(exe_path, sections, by_rva, starts, target, label):
    _n, vaddr, _size, raw_ptr, raw_size = next(s for s in sections if s[0] == ".text")
    with open(exe_path, "rb") as f:
        f.seek(raw_ptr)
        code = f.read(raw_size)

    candidates = set()
    for align in range(4):
        chunk = code[align:]
        chunk = chunk[: len(chunk) // 4 * 4]
        ints = array("i")
        ints.frombytes(chunk)
        for tail in (0, 1, 2, 4):
            want = target - vaddr - 4 - tail - align  # disp == want - 4k
            for k, disp in enumerate(ints):
                if disp + 4 * k == want:
                    candidates.add(align + 4 * k)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    found = {}
    for disp_off in sorted(candidates):
        for back in range(1, 8):
            start = disp_off - back
            if start < 0:
                continue
            ins = next(md.disasm(code[start:start + 16], vaddr + start), None)
            if ins is None:
                continue
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.address + ins.size + op.mem.disp == target:
                    found[ins.address] = f"{ins.mnemonic} {ins.op_str}"

    print(f"### references to {label} (RVA {target:08X})")
    for rva, text in sorted(found.items()):
        i = bisect.bisect_right(starts, rva) - 1
        print(f"  {rva:08X}  {text:<45} in {by_rva[starts[i]][:120]}")
    if not found:
        print("  (none found)")


if __name__ == "__main__":
    main()
