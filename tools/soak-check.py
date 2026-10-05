#!/usr/bin/env python3
"""soak-check.py — hold a soak log to design.md §13.5.

    tools/soak-check.py [--minutes 30] [--usb] out/soak/soak-YYYYMMDD-HHMMSS.log

Passes when the log shows one boot, heartbeats covering the whole run with
no gap, and, on every heartbeat:

  rt at least 0.995, with the run's mean at least 0.999;

and, since every counter is cumulative from boot, these zero on the last:

  late fields, slips, dropped snapshots, keys lost, i2c errors, ED holes,
  log lines dropped, underrun samples, late refills, the beeper's
  overflow.

rt is guest seconds per wall second over a heartbeat's five seconds. The
guest is paced by the audio queue (design.md §8), so it reads 1.000 or
0.999 with the last digit truncated; a guest falling behind shows as a
run of lower figures, which the floor catches.

Presents, keyboard polls and speaker edges must grow: a soak that
exercised nothing proves nothing. The workload must also run to the end:
after the program starts (soak.sh writes how many heartbeats came before,
beside the log), the speaker must move in every heartbeat, and the count
the program prints must rise from each screen dump to the next. Keys typed over the UART go to the
guest's matrix without passing the southbridge, so they are not key
events; the screen dumps soak.sh asks for must show the program reading
both of them instead (the column after the count: 16 for H, 8 for J).
Keys pressed on the PicoCalc during the run are reported, not required.
The consumed rate, the control quantity, is reported.

Power: each heartbeat carries the southbridge's gauge, and "charging"
there means USB power. A run that shows charging fails, unless --usb says
it was meant to be on USB. The converse does not hold: the bit is the
charger's, and clears once a full battery on USB has finished charging
(hardware-notes.md §6), so a run that never shows it still needs the
operator's word.
"""

import argparse
import re
import sys

HB = re.compile(
    r"heartbeat\s*: (\d+) fields, rt (\d+)\.(\d+), late (\d+) \(\+\d+\), slips (\d+) \| "
    r"(\d+) presents \((\d+) full, (\d+) dropped\).*?\| keys (\d+) \((\d+) lost\), "
    r"polls (\d+), i2c errors (\d+) \| ed holes (\d+), log dropped (\d+), "
    r"battery (\?|(\d+)%?( charging)?), (?:die )?(\?|-?\d+ C)")
AU = re.compile(
    r"audio\s*: (\d+) Hz consumed, queue \d+ \(low (\d+)\), underrun samples (\d+) \(\+\d+\), "
    r"late refills (\d+) \(\+\d+\), core overflow (\d+), speaker edges (\d+)")
PERF = re.compile(r"perf\s*:.*?core 0 busy ([0-9.]+)%")
SCREEN_ROW = re.compile(r"^\s*\|(\d+) (\d+)\s*\|$", re.M)
FIELD_HZ = 3250000 / 64896          # the 19K's field (design.md §11.1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--usb", action="store_true",
                    help="the run was meant to be on USB power, not battery")
    a = ap.parse_args()

    text = open(a.log, errors="replace").read()
    hbs = [m.groups() for m in HB.finditer(text)]
    aus = [m.groups() for m in AU.finditer(text)]
    fails = []

    boots = len(re.findall(r"firmware\s*:", text))
    if boots != 1:
        fails.append("%d boots in the log, not 1 (a reset during the run?)" % boots)
    if len(hbs) < 2 or not aus:
        print("FAIL: %d heartbeats in %s, too few to check" % (len(hbs), a.log))
        return 1
    last_hb = list(HB.finditer(text))[-1].start()
    if list(AU.finditer(text))[-1].start() < last_hb:
        fails.append("the last heartbeat has no audio line after it, so the "
                     "audio counters do not cover the end of the run")

    # Heartbeats come every five wall seconds; a capture can garble one,
    # which then fails to parse. The field count between two that did
    # parse says how many fell between them.
    fields = [int(h[0]) for h in hbs]
    span = (fields[-1] - fields[0]) / FIELD_HZ / 60
    if span < a.minutes:
        fails.append("heartbeats span %.1f minutes, less than %g" % (span, a.minutes))
    step = 5 * FIELD_HZ
    gaps, garbled = [], 0
    for x, y in zip(fields, fields[1:]):
        k = round((y - x) / step)
        if k < 1 or abs((y - x) - k * step) > 0.02 * step * k:
            gaps.append((x, y))
        else:
            garbled += k - 1
    if gaps:
        fails.append("%d irregular steps between heartbeats, first after field %d"
                     % (len(gaps), gaps[0][0]))
    if garbled > 2:
        fails.append("%d heartbeats missing from the capture" % garbled)

    rts = [int(h[1]) + int(h[2]) / 1000 for h in hbs[1:]]   # the first straddles the boot
    low = min(rts)
    mean = sum(rts) / len(rts)
    if low < 0.995:
        fails.append("rt fell to %.3f (heartbeat %d)" % (low, rts.index(low) + 1))
    if mean < 0.999:
        fails.append("mean rt %.4f" % mean)

    h, u = hbs[-1], aus[-1]
    zero = [
        ("late fields", int(h[3])),
        ("slips", int(h[4])),
        ("dropped snapshots", int(h[7])),
        ("keys lost", int(h[9])),
        ("i2c errors", int(h[11])),
        ("ED holes", int(h[12])),
        ("log lines dropped", int(h[13])),
        ("underrun samples", int(u[2])),
        ("late refills", int(u[3])),
        ("beeper overflow", int(u[4])),
    ]
    for name, v in zero:
        if v:
            fails.append("%s: %d" % (name, v))

    grew = [
        ("presents", int(hbs[0][5]), int(h[5])),
        ("keyboard polls", int(hbs[0][10]), int(h[10])),
        ("speaker edges", int(aus[0][5]), int(u[5])),
    ]
    for name, first, final in grew:
        if final <= first:
            fails.append("%s did not grow (%d to %d): not exercised" % (name, first, final))

    # The workload must run for the whole soak, not only at its start:
    # from the heartbeat after the program was started (soak.sh records
    # how many came before; without the record, the first with an edge),
    # the speaker must move in every heartbeat's window, and each screen
    # dump must show the program's count higher than the last one did.
    hb_pos = [m.start() for m in HB.finditer(text)]
    au_pos = [(m.start(), int(m.group(6))) for m in AU.finditer(text)]
    try:
        start = int(open(re.sub(r"\.log$", "", a.log) + ".start").read())
    except (OSError, ValueError):
        start = next((i for i, (_, e) in enumerate(au_pos) if e), len(au_pos))
        start = sum(1 for p in hb_pos if p < au_pos[min(start, len(au_pos) - 1)][0])
    begin = hb_pos[min(start + 1, len(hb_pos) - 1)]
    edges = [e for p, e in au_pos if p > begin]
    still = sum(1 for x, y in zip(edges, edges[1:]) if y <= x)
    if len(edges) < 2:
        fails.append("no audio lines after the program started")
    elif still:
        fails.append("the speaker was silent through %d of %d heartbeats after the "
                     "program started: it stopped" % (still, len(edges) - 1))
    dumps = [d for d in re.split(r"screen\s*:", text[begin:])[1:]]
    counts = [max((int(m.group(1)) for m in SCREEN_ROW.finditer(d)), default=0) for d in dumps]
    stalls = sum(1 for x, y in zip(counts, counts[1:]) if y <= x)
    if len(counts) < 2:
        fails.append("fewer than two screen dumps after the program started")
    elif stalls or not counts[0]:
        fails.append("the program's count did not rise between %d of %d screen dumps"
                     % (stalls, len(counts) - 1))

    charging = sum(1 for b in hbs if b[16])
    levels = [int(b[15]) for b in hbs if b[15]]
    if charging:
        power = "USB: charging on %d of %d heartbeats" % (charging, len(hbs))
        if not a.usb:
            fails.append("the battery was charging, so this was not on battery (--usb if meant)")
    elif levels:
        power = ("never charging, %d%% to %d%%: battery, or USB with the charge done"
                 % (levels[0], levels[-1]))
    else:
        power = "unknown: the gauge was never read"

    dies = [int(b[17].split()[0]) for b in hbs if b[17] != "?"]
    rates = sorted(int(x[0]) for x in aus[1:]) or [int(aus[0][0])]
    lows = [int(x[1]) for x in aus[1:]]
    busy = [float(m.group(1)) for m in PERF.finditer(text)][1:]
    keys = [int(m.group(2)) for m in SCREEN_ROW.finditer(text)]
    if keys.count(16) == 0 or keys.count(8) == 0:
        fails.append("the screen dumps do not show the program reading both typed keys "
                     "(%d H, %d J)" % (keys.count(16), keys.count(8)))
    pressed = int(h[8]) - int(hbs[0][8])

    print("soak: %s" % a.log)
    print("  %d heartbeats over %.1f minutes, fields %d to %d, one boot: %s"
          % (len(hbs), span, fields[0], fields[-1], "yes" if boots == 1 else "no (%d)" % boots))
    print("  rt min %.3f, mean %.4f" % (low, mean))
    for name, v in zero:
        print("  %-22s %d" % (name, v))
    for name, first, final in grew:
        print("  %-22s %d -> %d" % (name, first, final))
    print("  audio consumed         %d-%d Hz (the control quantity), queue low %d"
          % (rates[0], rates[-1], min(lows) if lows else -1))
    if busy:
        print("  core 0 busy            %.1f-%.1f %%" % (min(busy), max(busy)))
    if keys:
        print("  keys read on screen    %d rows dumped: %d H, %d J"
              % (len(keys), keys.count(16), keys.count(8)))
    if counts:
        print("  program count          %d screen dumps after the start, %d to %d"
              % (len(counts), counts[0], counts[-1]))
    print("  speaker moving         %d of %d heartbeats after the start"
          % (len(edges) - 1 - still if len(edges) > 1 else 0, max(len(edges) - 1, 0)))
    print("  PicoCalc key events    %d during the run (pressed by hand; not required)" % pressed)
    print("  power                  %s" % power)
    if dies:
        print("  die temperature        %d to %d C, %d C at the end (uncalibrated)"
              % (min(dies), max(dies), dies[-1]))
    if garbled:
        print("  %d heartbeat lines garbled in the capture; the counters are cumulative, "
              "so the later ones cover them" % garbled)
    if fails:
        print("FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print("PASS" if charging else "PASS (record whether it was on battery)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
