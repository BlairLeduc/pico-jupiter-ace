#!/usr/bin/env python3
"""trace-diff.py — design.md §13.4's trace diff against xAce.

Runs the committed ROM and the same keys under this project's core
(ace-trace) and under xAce's Z80 (xace-trace), and compares the
per-instruction traces, PC AF BC DE HL IX IY SP T and the bytes at PC:

    tools/trace-diff.py run [--keys '2 2 + .\\n'] [--fields N]
    tools/trace-diff.py diff OURS.trace REF.trace
    tools/trace-diff.py keys 'LOAD TUTTUT\\n' keys.txt   # for a tracer alone

`run` writes out/trace/{ours,ref}.trace and keys.txt. Build the tracers
first:

    cmake --build build/host --target ace-trace
    tools/trace/build-xace.sh

The two keep the same time: xace-trace raises INT over the same window
of each field, charges its acknowledge, and corrects xAce's known timing
errata, each against the Z80 manual (xace-trace.c). So there is no resync:
the traces agree line for line, or the diff stops at the first line where
they do not and says how.

  reset   A register differs only because the two power on with different
          values ($FFFF here, as FUSE does; 0 in xAce), up to the first
          line where both agree on it. Expected; counted per register.
          The alternate set is not traced, but EX AF,AF' and EXX bring
          it in, so whether a register has agreed goes with it.
  xy      F differs only in bits 3 and 5, which the Z80 manual leaves
          undefined and xAce does not model. Ours is held to FUSE and
          ZEXALL on them (design.md §5.4). Expected; counted, and the
          bits are left out of every other comparison.
  errata  F differs, after an instruction in XACE_FLAG_ERRATA, in only
          the bits xAce is known to get wrong there. Expected; counted.
  values  Same PC, but a register differs after both had agreed on it.
  path    The PC, T or the bytes at PC differ.

  halt    The ROM halted, and xAce ran on: its HALT is a NOP (z80ops.c,
          "no interrupt support"). The traces part for good here, so the
          diff ends, without failing, at the first HALT. The ROM halts
          once a word in VLIST ($0679), not at the prompt.

Exit status 0 when the traces agree to the end of the shorter, or to a
`halt`, are the same length unless they stopped at a halt, and have no
`values` or `path` difference; 1 otherwise.

The xAce timing errata are corrected in xace-trace.c, which keeps the two
clocks together; its flag errata are allowed for here.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGS = ("PC", "AF", "BC", "DE", "HL", "IX", "IY", "SP")

# design.md §2.4's half-rows, A8 low first, as (row, D-bit). '+' and '.'
# are SYMBOL SHIFT with K and M; a capital is SHIFT with its letter.
ROWS = ["\x01\x02zxc", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " mnbv"]
CELLS = {ch: (r, c) for r, row in enumerate(ROWS) for c, ch in enumerate(row) if ch > "\x02"}
SYMBOL = {"+": "k", ".": "m"}

BOOT_FIELDS = 10    # the ROM's prompt is up after under two (test_boot)
KEY_HOLD = 4        # fields a key is down; the ROM takes it on its third scan
KEY_EVERY = 8       # fields from one key to the next
LINE_EVERY = 50     # after ENTER, while the line runs


def keyscript(text):
    lines = ["# written by trace-diff.py from %r" % text]
    field = BOOT_FIELDS
    for ch in text:
        low = ch.lower()
        if low in CELLS:
            r, c = CELLS[low]
            mod = " shift" if ch != low else ""
            lines.append("%d %d %d %d%s" % (field, KEY_HOLD, r, c, mod))
        elif ch in SYMBOL:
            r, c = CELLS[SYMBOL[ch]]
            lines.append("%d %d %d %d sym" % (field, KEY_HOLD, r, c))
        else:
            sys.exit("trace-diff: no Ace key for %r" % ch)
        field += LINE_EVERY if ch == "\n" else KEY_EVERY
    return "\n".join(lines) + "\n", field


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            w = line.split()
            if len(w) == 10:
                out.append(w)
    return out


def fmt(w):
    return " ".join("%s=%s" % (n, v) for n, v in zip(REGS, w[:8])) + " T=%s %s" % (w[8], w[9])


# Where xAce's flags differ from ours for a known reason, found by this
# diff and then read in xAce's source: name -> F bits. After such an
# instruction (flag_erratum), F may differ in those bits only, until the
# two agree on F again.
XACE_FLAG_ERRATA = {
    # adchl in edops.c ORs in N; the Z80 manual's ADC HL,ss resets it.
    "ADC HL,ss": 0x02,
    # cbops.c's bit() never sets S. The manual calls S unknown after
    # BIT; FUSE, which ours passes, sets it for BIT 7 of a set bit.
    "BIT b,r": 0x80,
}
XY = 0x28


def flag_erratum(code):
    """The XACE_FLAG_ERRATA name for the instruction at `code`, the four
    bytes at PC as hex, or None."""
    b = bytes.fromhex(code)
    if b[0] == 0xED and b[1] in (0x4A, 0x5A, 0x6A, 0x7A):
        return "ADC HL,ss"
    if b[0] == 0xCB and 0x40 <= b[1] < 0x80:
        return "BIT b,r"
    if b[0] in (0xDD, 0xFD) and b[1] == 0xCB and 0x40 <= b[3] < 0x80:
        return "BIT b,r"
    return None


def diff(ours, ref, context):
    agreed = [False] * 8
    shadow = {1: False, 2: False, 3: False, 4: False}   # AF' BC' DE' HL'
    reset = [0] * 8
    xy = 0
    errata = {}             # opcode -> lines F differed by its erratum
    allow, allow_op = 0, None
    n = min(len(ours), len(ref))
    failure = None
    halted = None
    i = -1
    for i in range(n):
        o, r = ours[i], ref[i]
        if i > 0 and o[0] == ours[i - 1][0] and ours[i - 1][9][:2] == "76" and \
                int(r[0], 16) == (int(o[0], 16) + 1) & 0xFFFF:
            halted = i
            break
        if o[0] != r[0] or o[8] != r[8] or o[9] != r[9]:
            failure = ("path", i)
            break
        bad = []
        for k in range(1, 8):
            if k == 1:
                a, b = int(o[1], 16), int(r[1], 16)
                if a != b and (a ^ b) & ~XY == 0:
                    xy += 1
                if allow and (a ^ b) & ~XY:
                    if (a ^ b) & ~(XY | allow) == 0:
                        errata[allow_op] = errata.get(allow_op, 0) + 1
                        continue
                    allow = 0
                elif allow:
                    allow = 0       # they agree on F again
                same = (a ^ b) & ~XY == 0
            else:
                same = o[k] == r[k]
            if same:
                agreed[k] = True
            elif agreed[k]:
                bad.append(REGS[k])
            else:
                reset[k] += 1
        if bad:
            failure = ("values " + ",".join(bad), i)
            break
        op = o[9][:2]
        swapped = (1,) if op == "08" else (2, 3, 4) if op == "D9" else ()
        for k in swapped:
            agreed[k], shadow[k] = shadow[k], agreed[k]
        name = flag_erratum(o[9]) if op in ("ED", "DD", "FD", "CB") else None
        if name:
            allow_op, allow = name, XACE_FLAG_ERRATA[name]

    print("ours %d lines, ref %d lines; compared %d" % (len(ours), len(ref), i + 1))
    late = [REGS[k] for k in range(1, 8) if not agreed[k]]
    print("reset: " + (", ".join("%s %d lines" % (REGS[k], reset[k])
                                 for k in range(1, 8) if reset[k]) or "none")
          + ("; never agreed: " + ", ".join(late) if late else ""))
    print("xy: %d lines differ only in F's bits 3 and 5" % xy)
    for op, count in sorted(errata.items()):
        print("errata: after %s, %d lines (F bits $%02X)"
              % (op, count, XACE_FLAG_ERRATA[op]))
    if failure:
        kind, i = failure
        print("\n%s at line %d (columns: ours | ref)" % (kind, i))
        for k in range(max(0, i - context), min(n, i + 4)):
            mark = ">" if k == i else " "
            print("%s %9d  %s\n             %s" % (mark, k, fmt(ours[k]), fmt(ref[k])))
        return 1
    if halted is not None:
        print("halt at line %d: the ROM halted at $%s, where xAce does not; "
              "compared to here" % (halted, ours[halted][0]))
        return 0
    if len(ours) != len(ref):
        print("\nthe traces agree as far as the shorter goes, but differ in length")
        return 1
    print("no divergence")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    runp = sub.add_parser("run", help="trace both machines, then diff")
    runp.add_argument("--keys", default="", help="text typed after boot; \\n is ENTER")
    runp.add_argument("--fields", type=int, help="fields to run (default: boot, keys, 50 more)")
    runp.add_argument("--out", default=str(ROOT / "out" / "trace"))
    runp.add_argument("--ours", default=str(ROOT / "build" / "host" / "test" / "host" / "ace-trace"))
    runp.add_argument("--ref", default=str(ROOT / "out" / "trace" / "xace-trace"))
    keysp = sub.add_parser("keys", help="write the keyscript for some text, and say how many fields it needs")
    keysp.add_argument("text")
    keysp.add_argument("file")
    diffp = sub.add_parser("diff", help="diff two traces already written")
    diffp.add_argument("ours")
    diffp.add_argument("ref")
    for p in (runp, diffp):
        p.add_argument("--context", type=int, default=6, help="lines shown before a divergence")
    a = ap.parse_args()

    if a.cmd == "keys":
        script, end = keyscript(a.text.encode().decode("unicode_escape"))
        Path(a.file).write_text(script)
        print(end)
        return

    if a.cmd == "run":
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        text = a.keys.encode().decode("unicode_escape")
        script, end = keyscript(text)
        keys = out / "keys.txt"
        keys.write_text(script)
        fields = a.fields or end + 50
        rom = ROOT / "roms" / "ace.rom"
        for exe in (a.ours, a.ref):
            if not Path(exe).exists():
                sys.exit("trace-diff: %s is not built (see --help)" % exe)
        with open(out / "ours.trace", "w") as f:
            got = subprocess.run([a.ours, "-f", str(fields), "-k", str(keys), "-s"],
                                 stdout=f, stderr=subprocess.PIPE, text=True, check=True)
        screen = got.stderr.rstrip("\n")
        with open(out / "ref.trace", "w") as f:
            got = subprocess.run([a.ref, str(rom), "-f", str(fields), "-k", str(keys)],
                                 stdout=f, stderr=subprocess.PIPE, text=True, check=True)
        print("traced %d fields, keys %r" % (fields, text))
        print(got.stderr.rstrip("\n"))
        print("our screen at the end:")
        print("\n".join("  | " + line for line in screen.split("\n") if line) or "  (blank)")
        ours, ref = out / "ours.trace", out / "ref.trace"
    else:
        ours, ref = a.ours, a.ref

    sys.exit(diff(load(ours), load(ref), a.context))


if __name__ == "__main__":
    main()
