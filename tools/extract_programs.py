#!/usr/bin/env python3
"""Interleave the 68000 program ROMs from cybattlr.zip (or a folder) into main.bin / sound.bin."""
import sys, os, zipfile
src, out = sys.argv[1], sys.argv[2]
def get(name):
    if os.path.isdir(src):
        return open(os.path.join(src, name), 'rb').read()
    with zipfile.ZipFile(src) as z:
        for n in z.namelist():
            if os.path.basename(n).lower() == name:
                return z.read(n)
    raise SystemExit('missing ' + name)
def il(e, o):
    a, b = get(e), get(o); r = bytearray(len(a) * 2); r[0::2] = a; r[1::2] = b; return bytes(r)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'main.bin'), 'wb').write(il('cb_03.rom', 'cb_02.rom'))
open(os.path.join(out, 'sound.bin'), 'wb').write(il('cb_08.rom', 'cb_07.rom'))
