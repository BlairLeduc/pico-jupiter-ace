#!/usr/bin/env python3
"""ace-reference.py — .ace import against MAME, the reference (design.md
§13.4, §15.2 M11).

For each .ace file taken on a 19K machine (ACE32's RAMTOP word $8000), MAME's
jupace with 16K fitted loads it and runs ACE_DUMP_FIELDS fields
(tools/mame/ace-dump.lua), and so does this emulator's host build
(test_snap_ace's dump mode); then the registers, the screen and user RAM
$3C00-$7FFF are compared.

MAME loads part of the way through a field and this emulator between two, so
the machines run the same program a fraction of a field apart. For a file
taken in the ROM's key wait ($059B-$059D, where nearly all are) that leaves
three differences, which are not compared: where in the wait loop each PC is
(MAME's PC may name the middle of an instruction, so $059B-$059F); the
per-field counters FRAMES ($3C2B-$3C2D) and the key scan's ($3C27); and the
32 bytes below SP, where each interrupt leaves its return address and the
handler its pushes. Everything
else must be the same. A file taken while its program ran is reported, not
judged: a field's phase changes what a running program does. MAME refuses
files for smaller machines and reads none past $8000, so 3K, 35K and 51K
files are not compared here; test_snap_ace runs them.

MAME needs a ROM set: tools/mame/romset.sh builds it in out/mame/roms.

    tools/ace-reference.py DIR [--fields N] [--log out/m11-mame.log]
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COUNTERS = set(range(0x3C2B, 0x3C2E)) | {0x3C27}
WAIT = range(0x059B, 0x05A0)
DEAD = 32    # bytes below SP an interrupt and its handler's pushes may leave


def decode_header(path):
    """RAMTOP's word and PC, or (None, None)."""
    data = open(path, "rb").read()
    out = bytearray()
    i = 0
    while i < len(data) and len(out) < 0x120:
        b = data[i]
        i += 1
        if b == 0xED:
            if i >= len(data):
                return None, None          # cut off inside a marker
            n = data[i]
            if n == 0:
                break
            if i + 1 >= len(data):
                return None, None
            out += bytes([data[i + 1]]) * n
            i += 2
        else:
            out.append(b)
    if len(out) < 0x120:
        return None, None
    return out[0x80] | out[0x81] << 8, out[0x11C] | out[0x11D] << 8


def read_dump(path):
    regs, mem = {}, {}
    for line in open(path):
        a, b = line.split()
        if len(b) == 64:
            base = int(a, 16)
            for k in range(32):
                mem[base + k] = int(b[2 * k:2 * k + 2], 16)
        else:
            regs[a] = int(b, 16)
    return regs, mem


def run_mame(ace, out, fields):
    env = dict(os.environ, ACE_DUMP_OUT=out, ACE_DUMP_FIELDS=str(fields))
    r = subprocess.run(["mame", "jupace", "-rompath", "roms", "-ramsize", "16K",
                        "-dump", ace, "-video", "none", "-sound", "none", "-nothrottle",
                        "-autoboot_script", os.path.join(ROOT, "tools/mame/ace-dump.lua")],
                       cwd=os.path.join(ROOT, "out/mame"), env=env,
                       capture_output=True, text=True)
    return os.path.exists(out), r.stderr.strip().splitlines()[-1:] if r.stderr else []


def run_ours(ace, out, fields, exe):
    env = dict(os.environ, PICO_ACE_ACE_FILE=ace, PICO_ACE_ACE_DUMP=out,
               ACE_DUMP_FIELDS=str(fields))
    r = subprocess.run([exe], env=env, capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dir")
    p.add_argument("--fields", type=int, default=100)
    p.add_argument("--exe", default=os.path.join(ROOT, "build/host/test/host/test_snap_ace"))
    p.add_argument("--log")
    a = p.parse_args()

    log = open(a.log, "w") if a.log else None

    def say(s):
        print(s)
        if log:
            log.write(s + "\n")

    say(f"ace-reference: {a.dir}, {a.fields} fields after the load")
    same = differ = skipped = running = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in sorted(os.listdir(a.dir)):
            if not name.lower().endswith(".ace"):
                continue
            path = os.path.join(a.dir, name)
            ramtop, pc = decode_header(path)
            if ramtop != 0x8000:
                skipped += 1
                continue
            mo, oo = os.path.join(tmp, "mame.txt"), os.path.join(tmp, "ours.txt")
            for f in (mo, oo):
                if os.path.exists(f):
                    os.remove(f)
            ok_m, why_m = run_mame(path, mo, a.fields)
            ok_o, why_o = run_ours(path, oo, a.fields, a.exe)
            if not ok_m or not ok_o:
                differ += 1
                say(f"  FAIL {name}: MAME {'ok' if ok_m else why_m}, ours {'ok' if ok_o else why_o}")
                continue
            rm, mm = read_dump(mo)
            ro, mmo = read_dump(oo)
            waiting = pc in (0x059B, 0x059D)
            sp = min(rm["SP"], ro["SP"])
            bad = [r for r in rm if rm[r] != ro.get(r)
                   and not (waiting and r == "PC" and rm[r] in WAIT and ro[r] in WAIT)]
            addrs = [x for x in mm if mm[x] != mmo.get(x)
                     and not (waiting and (x in COUNTERS or sp - DEAD <= x < sp))]
            if not waiting:
                running += 1
                say(f"  ran  {name}: PC {pc:04X} when saved; {len(bad)} registers and "
                    f"{len(addrs)} bytes differ (not judged)")
            elif bad or addrs:
                differ += 1
                regs = " ".join(f"{r} {rm[r]:04X}/{ro.get(r, 0):04X}" for r in bad)
                first = ", ".join(f"${x:04X} {mm[x]:02X}/{mmo[x]:02X}" for x in addrs[:4])
                say(f"  DIFF {name}: {regs} {len(addrs)} bytes {first}")
            else:
                same += 1
                say(f"  same {name}: PC {rm['PC']:04X} SP {rm['SP']:04X}")
    say(f"{same} the same, {differ} different, {running} saved running (not judged), "
        f"{skipped} not 19K (not compared)")
    if not same:
        say("nothing was compared: no 19K file saved in the key wait")
    return 1 if differ or not same else 0


if __name__ == "__main__":
    sys.exit(main())
