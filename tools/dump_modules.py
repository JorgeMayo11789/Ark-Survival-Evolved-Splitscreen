"""List the modules (DLLs) recorded in a Windows minidump.

Usage: python dump_modules.py <file.dmp>
Reads only.
"""
import struct
import sys

MODULE_LIST_STREAM = 4


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"MDMP":
        sys.exit("not a minidump")
    (n_streams, dir_rva) = struct.unpack_from("<II", data, 8)
    for i in range(n_streams):
        stype, size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
        if stype != MODULE_LIST_STREAM:
            continue
        (count,) = struct.unpack_from("<I", data, rva)
        for j in range(count):
            base = rva + 4 + 108 * j
            image_base, image_size = struct.unpack_from("<QI", data, base)
            (name_rva,) = struct.unpack_from("<I", data, base + 20)
            (name_len,) = struct.unpack_from("<I", data, name_rva)
            name = data[name_rva + 4:name_rva + 4 + name_len].decode("utf-16-le")
            print(f"{image_base:016X}  {image_size:>10}  {name}")


if __name__ == "__main__":
    main()
