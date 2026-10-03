/* bench_zexdoc.S — ZEXDOC, as fetched by tools/fetch-test-suites.sh, in
 * the bench firmware's flash (design.md §15.2 M2). Assembled only when
 * the file is there; it is GPL, like this project, and never committed. */
    .section .rodata.bench_zexdoc, "a"
    .global bench_zexdoc
    .global bench_zexdoc_end
bench_zexdoc:
    .incbin PICO_ACE_ZEXDOC_PATH
bench_zexdoc_end:
