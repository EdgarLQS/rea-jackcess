#!/usr/bin/env python3
"""Creates the Shift-JIS (code page 932) Jet3 probe file used for the code-page cross-check.

The upstream V1997 samples only carry Windows-1252 text and Jackcess cannot write Jet3, so the
932 decode path is exercised the same way as 936: patch two ASCII cells of a copy of
`common1V1997.mdb` with same-length Shift-JIS bytes. `rea-jackcess canonical --code-page 932` on
the copy must then match `JackcessTableDump <copy> <out.json> windows-31J` table for table; both
decoders read the patched cells as `テストg` and `日本語测p`.

Usage: make_jet3_932_sample.py <common1V1997.mdb> <output.mdb>
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
        # Table1 row 2, column A: "abcdefg" -> テストg (6 Shift-JIS bytes + 'g', same 7-byte cell).
        # テ=8365 ス=8358 ト=8367.
        (b"abcdefg", bytes.fromhex("836583588367") + b"g"),
        # Table1 row 2, column B: "hijklmnop" -> 日本語测p (8 bytes + 'p', same 9-byte cell).
        # 日=93FA 本=967B 語=8CEA 测=91AA.
        (b"hijklmnop", bytes.fromhex("93fa967b8cea91aa") + b"p"),
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
