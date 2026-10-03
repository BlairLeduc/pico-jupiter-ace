# Host tests (design.md §13). Every one of these builds src/core/ with the
# system compiler and no Pico SDK. One binary per area; exit 77 is a skip.

set(PICO_ACE_HOST_TESTS
    test_skeleton
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
