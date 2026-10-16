"""Dump public/global symbols from an MSVC PDB (MSF 7.0) with RVAs.

Usage: python pdb_dump.py <game.exe> <game.pdb> <out.tsv>

Reads only. No third-party packages: parses the MSF container and the DBI
symbol record stream directly, maps segment:offset to RVA using the exe's
section headers, and demangles names with Windows' built-in dbghelp.dll.
"""
import ctypes
import struct
import sys

MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
S_PUB32 = 0x110E
S_GDATA32 = 0x110D
S_LDATA32 = 0x110C
KIND_NAMES = {S_PUB32: "pub", S_GDATA32: "gdata", S_LDATA32: "ldata"}


class Msf:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = self.f.read(56)
        if sb[:32] != MSF_MAGIC:
            raise ValueError("not an MSF 7.0 PDB")
        (self.block_size, _fpm, _nblocks, dir_bytes, _unk, block_map_addr) = struct.unpack_from("<6I", sb, 32)
        n_dir_blocks = -(-dir_bytes // self.block_size)
        self.f.seek(block_map_addr * self.block_size)
        dir_block_ids = struct.unpack(f"<{n_dir_blocks}I", self.f.read(4 * n_dir_blocks))
        directory = b"".join(self._block(b) for b in dir_block_ids)[:dir_bytes]

        (n_streams,) = struct.unpack_from("<I", directory, 0)
        sizes = struct.unpack_from(f"<{n_streams}I", directory, 4)
        pos = 4 + 4 * n_streams
        self.streams = []
        for size in sizes:
            if size == 0xFFFFFFFF:
                self.streams.append((0, ()))
                continue
            nb = -(-size // self.block_size)
            blocks = struct.unpack_from(f"<{nb}I", directory, pos)
            pos += 4 * nb
            self.streams.append((size, blocks))

    def _block(self, idx):
        self.f.seek(idx * self.block_size)
        return self.f.read(self.block_size)

    def stream(self, i):
        size, blocks = self.streams[i]
        return b"".join(self._block(b) for b in blocks)[:size]


def exe_sections(path):
    with open(path, "rb") as f:
        data = f.read(4096)
    (pe_off,) = struct.unpack_from("<I", data, 0x3C)
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        raise ValueError("not a PE file")
    (n_sections,) = struct.unpack_from("<H", data, pe_off + 6)
    (opt_size,) = struct.unpack_from("<H", data, pe_off + 20)
    sec_off = pe_off + 24 + opt_size
    out = []
    for i in range(n_sections):
        base = sec_off + 40 * i
        name = data[base:base + 8].rstrip(b"\x00").decode("ascii", "replace")
        (vaddr,) = struct.unpack_from("<I", data, base + 12)
        out.append((name, vaddr))
    return out


_undecorate = ctypes.windll.dbghelp.UnDecorateSymbolName
_undecorate.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
_buf = ctypes.create_string_buffer(4096)


def demangle(name):
    if not name.startswith("?"):
        return name
    n = _undecorate(name.encode("latin-1"), _buf, len(_buf), 0)
    return _buf.value[:n].decode("latin-1") if n else name


def main():
    exe_path, pdb_path, out_path = sys.argv[1:4]
    sections = exe_sections(exe_path)
    msf = Msf(pdb_path)

    dbi = msf.stream(3)
    (sym_record_stream,) = struct.unpack_from("<H", dbi, 20)
    records = msf.stream(sym_record_stream)

    count = 0
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("rva\tkind\tname\tmangled\n")
        pos = 0
        end = len(records)
        while pos + 4 <= end:
            reclen, kind = struct.unpack_from("<HH", records, pos)
            body = pos + 4
            pos += 2 + reclen
            if kind == S_PUB32:
                _flags, offset, seg = struct.unpack_from("<IIH", records, body)
                name_at = body + 10
            elif kind in (S_GDATA32, S_LDATA32):
                _type, offset, seg = struct.unpack_from("<IIH", records, body)
                name_at = body + 10
            else:
                continue
            name_end = records.index(b"\x00", name_at)
            mangled = records[name_at:name_end].decode("latin-1")
            if not 1 <= seg <= len(sections):
                continue
            rva = sections[seg - 1][1] + offset
            out.write(f"{rva:08X}\t{KIND_NAMES[kind]}\t{demangle(mangled)}\t{mangled}\n")
            count += 1
    print(f"sections: {[s[0] for s in sections]}")
    print(f"wrote {count} symbols to {out_path}")


if __name__ == "__main__":
    main()
