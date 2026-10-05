#!/usr/bin/env bash
# perf-summary.sh — reduce perf-run.sh's logs to one line per workload
# (design.md §14, §15.2 M12).
#
#   tools/perf-summary.sh out/m12/t0-a out/m12/t2-a ...
#
# Averages the `perf` heartbeats taken after the program was started
# (perf-run.sh records where that was), dropping the first, which
# straddles the start. Core 0 busy is the share that counts: the guest,
# the keys and the snapshot, without the wait on the audio queue. The
# consumed rate is the control: it is the PWM wrap, and it must read the
# same in every row. Underruns and late refills are the run's growth.
set -euo pipefail

# The capture opens with line noise from the reset, which is not UTF-8.
export LC_ALL=C

for dir in "$@"; do
    for log in "$dir"/*.log; do
        case "$log" in *.flash.log) continue ;; esac
        w="$(basename "$log" .log)"
        start="$(cat "$dir/$w.start" 2>/dev/null || echo 0)"
        awk -v dir="$dir" -v w="$w" -v start="$start" '
            function num(re, skip_l, skip_r) {
                if (!match($0, re)) return ""
                return substr($0, RSTART + skip_l, RLENGTH - skip_l - skip_r) + 0
            }
            / perf / {
                n++
                if (n <= start + 1) next
                b = num("busy [0-9.]+%", 5, 1)
                c = num("[0-9.]+ host cycles", 0, 12)
                t = num("[0-9.]+ T/insn", 0, 7)
                i = num("[0-9]+ insns", 0, 6)
                h = num("[0-9]+ halts", 0, 6)
                if (b == "") next
                sb += b; sc += c; st += t; si += i; sh += h; k++
                if (k == 1 || b < bmin) bmin = b
                if (k == 1 || b > bmax) bmax = b
            }
            / audio / && n > start + 1 {
                r = num("[0-9]+ Hz consumed", 0, 12)
                u = num("underrun samples [0-9]+", 17, 0)
                l = num("late refills [0-9]+", 13, 0)
                if (kr == 0) { u0 = u; l0 = l }
                sr += r; kr++; u1 = u; l1 = l
            }
            END {
                if (!k) { printf "%-22s %-8s no steady heartbeats\n", dir, w; exit }
                printf "%-22s %-8s n=%d  busy %5.1f%% (%.1f-%.1f)  %6.1f host cyc/insn  %5.2f T/insn  %7.0f insns  %8.0f halts  audio %d Hz  underruns +%d  late +%d\n",
                       dir, w, k, sb / k, bmin, bmax, sc / k, st / k, si / k, sh / k,
                       kr ? sr / kr : 0, u1 - u0, l1 - l0
            }' "$log"
    done
done
