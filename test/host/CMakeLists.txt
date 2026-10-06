# Host tests (design.md §13). Every one of these builds src/core/ with the
# system compiler and no Pico SDK. One binary per area; exit 77 is a skip.

set(PICO_ACE_HOST_TESTS
    test_keymap
    test_settings
    test_skeleton
    test_status
    test_z80_behaviour
    test_z80_fuse
)

foreach(t ${PICO_ACE_HOST_TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE pico_ace_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${PICO_ACE_WARNINGS})
    target_compile_definitions(${t} PRIVATE
        PICO_ACE_ROM_PATH="${CMAKE_SOURCE_DIR}/roms/ace.rom"
        PICO_ACE_DEFAULT_TEST_SUITES="${CMAKE_SOURCE_DIR}/test/suites")
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# The tests that run the real ROM (§13.3). It is embedded in the core,
# so they never skip.
set(PICO_ACE_GUEST_TESTS
    test_audio
    test_boot
    test_bus
    test_cassette
    test_field
    test_golden
    test_halt
    test_keyboard
    test_render
    test_snappool
    test_snap_ace
    test_snapshot
    test_tape
    test_wait
)
foreach(t ${PICO_ACE_GUEST_TESTS})
    add_executable(${t} ${t}.c guest.c)
    target_link_libraries(${t} PRIVATE pico_ace_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${PICO_ACE_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The pitch is measured in double precision; libm is separate on Linux.
if (UNIX AND NOT APPLE)
    target_link_libraries(test_audio PRIVATE m)
endif()

# The committed golden images (design.md §7.6).
target_compile_definitions(test_golden PRIVATE
    PICO_ACE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# The build's ROM check, run on a corrupted copy (§10.2).
add_executable(test_rom_embed test_rom_embed.c)
target_link_libraries(test_rom_embed PRIVATE pico_ace_core)
target_include_directories(test_rom_embed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_rom_embed PRIVATE ${PICO_ACE_WARNINGS})
target_compile_definitions(test_rom_embed PRIVATE
    PICO_ACE_ROM_PATH="${PICO_ACE_ROM}"
    PICO_ACE_CMAKE="${CMAKE_COMMAND}"
    PICO_ACE_EMBED_ROM="${PICO_ACE_EMBED_ROM}")
add_test(NAME test_rom_embed COMMAND test_rom_embed)

# ZEXDOC and ZEXALL: one binary, two tests, each minutes long (§5.4). The
# programs are fetched, not committed; without them both report skipped.
add_executable(test_z80_zex test_z80_zex.c)
target_link_libraries(test_z80_zex PRIVATE pico_ace_core)
target_include_directories(test_z80_zex PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_z80_zex PRIVATE ${PICO_ACE_WARNINGS})
target_compile_definitions(test_z80_zex PRIVATE
    PICO_ACE_DEFAULT_TEST_SUITES="${CMAKE_SOURCE_DIR}/test/suites")
foreach(zex zexdoc zexall)
    add_test(NAME test_z80_${zex} COMMAND test_z80_zex ${zex}.com)
    set_tests_properties(test_z80_${zex} PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 3600 LABELS long)
endforeach()

# M2's workloads (design.md §15.2 M2): the forth program always; the start
# of ZEXDOC when the suites have been fetched.
add_executable(test_bench test_bench.c)
target_link_libraries(test_bench PRIVATE pico_ace_bench)
target_include_directories(test_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_bench PRIVATE ${PICO_ACE_WARNINGS})
target_compile_definitions(test_bench PRIVATE
    PICO_ACE_DEFAULT_TEST_SUITES="${CMAKE_SOURCE_DIR}/test/suites")
add_test(NAME test_bench_forth COMMAND test_bench)
add_test(NAME test_bench_zexdoc COMMAND test_bench zexdoc.com)
set_tests_properties(test_bench_forth test_bench_zexdoc PROPERTIES SKIP_RETURN_CODE 77)

# Our half of the trace diff (design.md §13.4): a tool, not a test.
# tools/trace-diff.py runs it beside tools/trace/build-xace.sh's reference.
add_executable(ace-trace ${CMAKE_SOURCE_DIR}/tools/trace/ace-trace.c)
target_link_libraries(ace-trace PRIVATE pico_ace_core)
target_include_directories(ace-trace PRIVATE ${CMAKE_SOURCE_DIR}/tools/trace)
target_compile_options(ace-trace PRIVATE ${PICO_ACE_WARNINGS})
