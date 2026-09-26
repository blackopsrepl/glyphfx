#!/usr/bin/env python3
"""Report the first byte divergence between two streams, with a decoded view."""
import sys


def preview(data, start, end):
    chunk = data[max(0, start - 20):end + 20]
    return chunk.decode("utf-8", "backslashreplace").replace("\x1b", "\\x1b").replace("\n", "\\n")


def main():
    if len(sys.argv) != 3:
        print("usage: differ.py oracle subject", file=sys.stderr)
        return 2
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            print(f"first divergence at byte {i}: oracle=0x{a[i]:02x} subject=0x{b[i]:02x}")
            print(f"  oracle : {preview(a, i, i)}")
            print(f"  subject: {preview(b, i, i)}")
            return 1
    if len(a) != len(b):
        print(f"length mismatch: oracle={len(a)} subject={len(b)} at byte {n}")
        print(f"  extra oracle : {preview(a, n, n)}")
        print(f"  extra subject: {preview(b, n, n)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
