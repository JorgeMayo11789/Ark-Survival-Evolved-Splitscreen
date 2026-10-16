"""For each named function, find the shortest run of whole instructions >= 14 bytes at its start that can be
relocated into a trampoline (no RIP-relative operands, no branches/calls), and print a C++ table row.

Usage: python prologues.py <game.exe> <symbols.tsv> <exact-demangled-name-prefix> [...]
Reads only.
"""
import os
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import load_sections, load_symbols, rva_to_file  # noqa: E402

MIN_BYTES = 14


def main():
    exe_path, sym_path, *names = sys.argv[1:]
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    with open(exe_path, "rb") as f:
        for wanted in names:
            matches = [(rva, n) for rva, n in by_rva.items() if wanted in n and "`" not in n]
            if len(matches) != 1:
                print(f"// {wanted}: {len(matches)} matches, skipped")
                continue
            rva, name = matches[0]
            f.seek(rva_to_file(sections, rva))
            code = f.read(64)
            total = 0
            ok = True
            for ins in md.disasm(code, rva):
                if any(ins.group(g) for g in (CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET)) or any(
                    op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands
                ):
                    ok = False
                    break
                total += ins.size
                if total >= MIN_BYTES:
                    break
            if not ok or total < MIN_BYTES:
                print(f"// {name}: no relocatable prologue (got {total} bytes)")
                continue
            body = ", ".join(f"0x{b:02X}" for b in code[:total])
            print(f"// {name}\n{{0x{rva:08X}, {total}, {{{body}}}}},")


if __name__ == "__main__":
    main()
