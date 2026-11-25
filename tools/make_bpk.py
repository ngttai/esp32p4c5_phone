#!/usr/bin/env python3
"""Pack a package folder (manifest.json + files) into a .bpk (ZIP, deflate): make_bpk.py <folder> <out.bpk>"""
import os, sys, zipfile

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isfile(os.path.join(src, "manifest.json")):
        sys.exit("manifest.json not found in " + src)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _, files in os.walk(src):
            for name in sorted(files):
                full = os.path.join(root, name)
                z.write(full, os.path.relpath(full, src))
    print(out, os.path.getsize(out), "bytes")

main()
