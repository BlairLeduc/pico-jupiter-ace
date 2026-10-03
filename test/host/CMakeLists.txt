# Host tests (design.md §13). Every one of these builds src/core/ with the
# system compiler and no Pico SDK. One binary per area; exit 77 is a skip.

set(PICO_ACE_HOST_TESTS
    test_skeleton
)

foreach(t ${PICO_ACE_HOST_TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE pico_ace_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${PICO_ACE_WARNINGS})
    target_compile_definitions(${t} PRIVATE
        PICO_ACE_ROM_PATH="${CMAKE_SOURCE_DIR}/roms/ace.rom")
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
