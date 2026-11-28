#!/usr/bin/env python3
"""Creates the GBK (code page 936) Jet3 probe file used for the code-page cross-check.

The upstream V1997 samples only carry Windows-1252 text and Jackcess cannot write Jet3, so the
936 decode path is exercised by patching two ASCII cells of a copy of `common1V1997.mdb` with
same-length GBK bytes. `rea-jackcess canonical --code-page 936` on the copy must then match
`JackcessTableDump <copy> <out.json> GBK` table for table; both decoders read the patched cells
as `中文数g` and `数据库测p`.

Usage: make_jet3_936_sample.py <common1V1997.mdb> <output.mdb>
"""
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    source, destination = Path(sys.argv[1]), Path(sys.argv[2])
    data = bytearray(source.read_bytes())

    patches = [
        # Table1 row 2, column A: "abcdefg" -> 中文数g (6 GBK bytes + 'g', same 7-byte cell).
        (b"abcdefg", bytes.fromhex("d6d0cec4cafd") + b"g"),
        # Table1 row 2, column B: "hijklmnop" -> 数据库测p (8 GBK bytes + 'p', same 9-byte cell).
        (b"hijklmnop", bytes.fromhex("cafdbeddbfe2b2e2") + b"p"),
    ]
    for old, new in patches:
        occurrences = data.count(old)
        if occurrences != 1:
            print(f"expected exactly one occurrence of {old!r}, found {occurrences}", file=sys.stderr)
            return 1
        index = data.index(old)
        data[index:index + len(old)] = new

    destination.write_bytes(data)
    print(f"wrote {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
