"""Disassemble named functions from a PE using a symbol TSV from pdb_dump.py.

Usage: python disasm.py <game.exe> <symbols.tsv> <name-substring> [<name-substring> ...]

Each argument is matched against demangled names; every matching function is
disassembled from its RVA up to the next known symbol. Calls, jumps and
RIP-relative memory references are annotated with symbol names when known.
Reads only.
"""
import bisect
import os
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

MAX_BYTES = int(os.environ.get("DISASM_MAX_BYTES", "0x1000"), 0)  # per function


def load_sections(path):
    with open(path, "rb") as f:
        head = f.read(4096)
    (pe_off,) = struct.unpack_from("<I", head, 0x3C)
    (n_sections,) = struct.unpack_from("<H", head, pe_off + 6)
    (opt_size,) = struct.unpack_from("<H", head, pe_off + 20)
    sec_off = pe_off + 24 + opt_size
    sections = []
    for i in range(n_sections):
        base = sec_off + 40 * i
        name = head[base:base + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", head, base + 8)
        sections.append((name, vaddr, max(vsize, raw_size), raw_ptr, raw_size))
    return sections


def rva_to_file(sections, rva):
    for _name, vaddr, size, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + size and rva - vaddr < raw_size:
            return raw_ptr + (rva - vaddr)
    raise ValueError(f"RVA {rva:X} not in file-backed section")


def load_symbols(path):
    by_rva = {}
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            rva_s, kind, name, _mangled = line.rstrip("\n").split("\t", 3)
            if kind != "pub":
                continue
            rva = int(rva_s, 16)
            by_rva.setdefault(rva, name)
    return by_rva


def short(name, limit=110):
    return name if len(name) <= limit else name[:limit] + "..."


def main():
    exe_path, sym_path, *queries = sys.argv[1:]
    sections = load_sections(exe_path)
    by_rva = load_symbols(sym_path)
    starts = sorted(by_rva)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    with open(exe_path, "rb") as exe:
        for query in queries:
            matches = [(rva, n) for rva, n in by_rva.items() if query in n and "`" not in n]
            if not matches:
                print(f"### no symbol matching {query!r}\n")
                continue
            for rva, name in sorted(matches):
                nxt = bisect.bisect_right(starts, rva)
                end = starts[nxt] if nxt < len(starts) else rva + MAX_BYTES
                length = min(end - rva, MAX_BYTES)
                exe.seek(rva_to_file(sections, rva))
                code = exe.read(length)
                print(f"### {name}\n### RVA {rva:08X}, {length} bytes\n")
                for ins in md.disasm(code, rva):
                    note = ""
                    for op in ins.operands:
                        target = None
                        if op.type == X86_OP_IMM and ins.mnemonic.startswith(("call", "j")):
                            target = op.imm
                        elif op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                            target = ins.address + ins.size + op.mem.disp
                        if target is not None:
                            label = by_rva.get(target)
                            if label is None:
                                i = bisect.bisect_right(starts, target) - 1
                                if i >= 0 and target - starts[i] < 0x40 and not ins.mnemonic.startswith("j"):
                                    label = f"{by_rva[starts[i]]}+0x{target - starts[i]:X}"
                            if label:
                                note = f"    ; {short(label)}"
                    print(f"  {ins.address:08X}  {ins.bytes.hex(' '):<30} {ins.mnemonic:<7} {ins.op_str}{note}")
                print()


if __name__ == "__main__":
    main()
