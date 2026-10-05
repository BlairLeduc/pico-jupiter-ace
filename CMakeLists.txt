# pico-ace — one source tree, two targets (design.md §4.1):
#   - the UF2 for the PicoCalc (default)
#   - a host test build that compiles src/core/ with the system compiler
#     and no Pico SDK, behind -DPICO_ACE_HOST=ON, run under CTest.
#
# The core building clean under -Wall -Wextra -Werror on both toolchains
# is the mechanism that keeps SDK dependencies from leaking downwards.

cmake_minimum_required(VERSION 3.18)

option(PICO_ACE_HOST "Build the portable core and its host tests, with no Pico SDK" OFF)

# Every fixed capacity lives in src/core/config.h; nothing here duplicates one.
set(PICO_ACE_CORE_SOURCES
    src/core/ace.c
    src/core/beeper.c
    src/core/cassette.c
    src/core/font.c
    src/core/keymap_picocalc.c
    src/core/keymatrix.c
    src/core/render.c
    src/core/romfont.c
    src/core/settings.c
    src/core/sha1.c
    src/core/snap_ace.c
    src/core/snappool.c
    src/core/snapshot.c
    src/core/tape.c
    src/core/z80.c
)

# The Ace ROM, embedded from roms/ace.rom at build time and refused unless
# its SHA-1 is the one ace_rom.h names (design.md §10.2).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ace_rom.h PICO_ACE_ROM_SHA1
     REGEX "^#define ACE_ROM_SHA1 ")
string(REGEX REPLACE ".*\"([0-9a-f]+)\".*" "\\1" PICO_ACE_ROM_SHA1 "${PICO_ACE_ROM_SHA1}")
set(PICO_ACE_ROM ${CMAKE_CURRENT_SOURCE_DIR}/roms/ace.rom)
set(PICO_ACE_EMBED_ROM ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_rom.cmake)

set(PICO_ACE_WARNINGS -Wall -Wextra -Werror)

# M2's workloads: portable like the core, run by a host test and by the
# bench firmware (design.md §15.2 M2).
function(pico_ace_bench_library)
    add_library(pico_ace_bench STATIC src/bench/bench.c)
    target_include_directories(pico_ace_bench PUBLIC src/bench)
    target_link_libraries(pico_ace_bench PUBLIC pico_ace_core)
    target_compile_options(pico_ace_bench PRIVATE ${PICO_ACE_WARNINGS})
endfunction()

# The core as a library both targets link. Headers only while it has no
# sources; a STATIC library, built with our warnings, once it has.
function(pico_ace_core_library)
    set(rom_c ${CMAKE_BINARY_DIR}/generated/ace_rom.c)
    add_custom_command(
        OUTPUT ${rom_c}
        COMMAND ${CMAKE_COMMAND} -DROM=${PICO_ACE_ROM} -DSHA1=${PICO_ACE_ROM_SHA1}
                -DOUT=${rom_c} -P ${PICO_ACE_EMBED_ROM}
        DEPENDS ${PICO_ACE_ROM} ${PICO_ACE_EMBED_ROM}
        COMMENT "pico-ace: embedding roms/ace.rom, checked by SHA-1"
        VERBATIM)
    if (PICO_ACE_CORE_SOURCES)
        add_library(pico_ace_core STATIC ${PICO_ACE_CORE_SOURCES} ${rom_c})
        target_include_directories(pico_ace_core PUBLIC src/core)
        target_compile_options(pico_ace_core PRIVATE ${PICO_ACE_WARNINGS})
    else()
        add_library(pico_ace_core INTERFACE)
        target_include_directories(pico_ace_core INTERFACE src/core)
    endif()
endfunction()

if (PICO_ACE_HOST)
    project(pico_ace_host C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)

    pico_ace_core_library()
    pico_ace_bench_library()

    enable_testing()
    add_subdirectory(test/host)
else()
    # One pico2 image for every RP2350 board (design.md §3.1).
    if (NOT DEFINED PICO_BOARD)
        set(PICO_BOARD pico2)
    endif()
    if (NOT DEFINED PICO_PLATFORM AND PICO_BOARD STREQUAL "pico2")
        set(PICO_PLATFORM rp2350)
    endif()

    include(cmake/pico_sdk_import.cmake)

    project(pico_ace C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    pico_sdk_init()

    pico_ace_core_library()
    pico_ace_bench_library()

    # PICO_ACE_UART=OFF is the build that ships: nothing on UART1, so GP4
    # and GP5 are free. ON, the default, is the development build, which
    # uart-log.sh and flash.sh's checks need (hardware-notes.md §2.7).
    option(PICO_ACE_UART "Log over UART1 on GP4/GP5 (development)" ON)
    if (PICO_ACE_UART)
        set(PICO_ACE_UART_ON 1)
    else()
        set(PICO_ACE_UART_ON 0)
    endif()

    # PICO_ACE_AUDIO=OFF paces on the microsecond timer and leaves the PWM
    # pins alone; ON, the default, paces on the audio queue (design.md §8).
    option(PICO_ACE_AUDIO "Sound on GP26/GP27, and pacing on its queue" ON)
    if (PICO_ACE_AUDIO)
        set(PICO_ACE_AUDIO_ON 1)
    else()
        set(PICO_ACE_AUDIO_ON 0)
    endif()

    # PICO_ACE_HALT_SKIP=OFF interprets every HALT repeat, the control for
    # the fast-forward (design.md §5.3, §15.2 M12).
    option(PICO_ACE_HALT_SKIP "Skip HALT repeats rather than interpret them" ON)
    if (PICO_ACE_HALT_SKIP)
        set(PICO_ACE_HALT_SKIP_ON 1)
    else()
        set(PICO_ACE_HALT_SKIP_ON 0)
    endif()

    # PICO_ACE_TURBO=OFF paces the guest while the tape plays, the control
    # for turbo's speed-up and for core 0's share during a load (design.md
    # §11.2, §15.2 M13).
    option(PICO_ACE_TURBO "Run unpaced while the tape plays or records" ON)
    if (PICO_ACE_TURBO)
        set(PICO_ACE_TURBO_ON 1)
    else()
        set(PICO_ACE_TURBO_ON 0)
    endif()

    # PICO_ACE_WAIT=OFF leaves the waiting mirrors on the fast path and
    # holds nothing, the control for the wait-state model (design.md §6.4,
    # §15.2 M14).
    option(PICO_ACE_WAIT "Hold the CPU on the waiting mirrors during the display" ON)
    if (PICO_ACE_WAIT)
        set(PICO_ACE_WAIT_ON 1)
    else()
        set(PICO_ACE_WAIT_ON 0)
    endif()

    # How much of the core's hot path runs from SRAM (src/core/hot.h).
    # Tier 2 ships: M12 measured it against 0 and 1 (design.md §3.2). The
    # core library needs it too: that is where the marked functions are.
    # A build directory configured before M12 keeps its cached 0.
    set(PICO_ACE_RAM_TIER 2 CACHE STRING "Hot-code SRAM tier, 0-2 (src/core/hot.h)")
    target_compile_definitions(pico_ace_core PRIVATE PICO_ACE_RAM_TIER=${PICO_ACE_RAM_TIER})

    # The firmware's version, git describe --always --dirty, for the
    # banner. A target that always runs, so it is taken at build time; the
    # header only changes when the text does.
    set(PICO_ACE_GENERATED ${CMAKE_BINARY_DIR}/generated)
    add_custom_target(pico_ace_version ALL
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DOUT=${PICO_ACE_GENERATED}/pico_ace_version.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
        BYPRODUCTS ${PICO_ACE_GENERATED}/pico_ace_version.h
        COMMENT "pico-ace: version from git describe"
        VERBATIM)

    get_filename_component(PICO_ACE_TOOLCHAIN_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    find_program(PICO_ACE_SIZE arm-none-eabi-size HINTS ${PICO_ACE_TOOLCHAIN_BIN} REQUIRED)

    # What every image shares: the board, the clocks, the UART and the
    # banner.
    function(pico_ace_firmware target)
        target_include_directories(${target} PRIVATE src/port ${PICO_ACE_GENERATED})
        target_compile_options(${target} PRIVATE ${PICO_ACE_WARNINGS})
        add_dependencies(${target} pico_ace_version)

        # clk_peri does not follow clk_sys by default; the SPI baud and the
        # audio carrier are re-applied in code as well (hardware-notes.md §3).
        target_compile_definitions(${target} PRIVATE
            PICO_ACE_UART=${PICO_ACE_UART_ON}
            PICO_ACE_RAM_TIER=${PICO_ACE_RAM_TIER}
            PICO_ACE_HALT_SKIP=${PICO_ACE_HALT_SKIP_ON}
            PICO_ACE_TURBO=${PICO_ACE_TURBO_ON}
            PICO_ACE_WAIT=${PICO_ACE_WAIT_ON}
            PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
            # UART1 at 115200 8-N-1, TX GP4 / RX GP5 (hardware-notes.md §2.7).
            PICO_DEFAULT_UART=1
            PICO_DEFAULT_UART_TX_PIN=4
            PICO_DEFAULT_UART_RX_PIN=5
        )

        target_link_libraries(${target}
            pico_ace_core
            pico_stdlib
            pico_unique_id
            hardware_adc
            hardware_clocks
            hardware_vreg
        )

        pico_enable_stdio_uart(${target} ${PICO_ACE_UART_ON})
        pico_enable_stdio_usb(${target} 0)

        pico_add_extra_outputs(${target})

        # The image's size on every build (design.md §3.3, §15.2 M0).
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${PICO_ACE_SIZE} $<TARGET_FILE:${target}>
            VERBATIM)
    endfunction()

    # FatFs is not in the tree (THIRD-PARTY.md). The SDK carries a copy for
    # TinyUSB, and ff.h includes "ffconf.h" from its own directory, so the
    # files are copied into the build tree beside our src/port/fatfs/ffconf.h
    # rather than compiled in place (which would find TinyUSB's config).
    set(PICO_ACE_FATFS_SRC ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source)
    set(PICO_ACE_FATFS_DIR ${CMAKE_BINARY_DIR}/fatfs)
    foreach(f ff.c ff.h ffunicode.c diskio.h)
        if (NOT EXISTS ${PICO_ACE_FATFS_SRC}/${f})
            message(FATAL_ERROR "pico-ace: FatFs ${f} not found in ${PICO_ACE_FATFS_SRC}")
        endif()
        configure_file(${PICO_ACE_FATFS_SRC}/${f} ${PICO_ACE_FATFS_DIR}/${f} COPYONLY)
    endforeach()
    configure_file(src/port/fatfs/ffconf.h ${PICO_ACE_FATFS_DIR}/ffconf.h COPYONLY)

    add_library(pico_ace_fatfs STATIC
        ${PICO_ACE_FATFS_DIR}/ff.c
        ${PICO_ACE_FATFS_DIR}/ffunicode.c
    )
    target_include_directories(pico_ace_fatfs PUBLIC ${PICO_ACE_FATFS_DIR})
    # Third-party code: built as it comes, without our -Werror.

    # The machine a run over the UART boots, over the settings file's
    # (EL §8.7): 3k, 19k or 51k, or empty for the file's.
    set(PICO_ACE_BOOT_RAM "" CACHE STRING "Boot this RAM size over the card's (3k, 19k, 51k)")
    # A tape put in the deck at boot over the file's boot_tape: a bare
    # name in /ace/tapes/ or a path, or empty for the file's.
    set(PICO_ACE_BOOT_TAPE "" CACHE STRING "Put this tape in the deck at boot, over the card's")

    add_executable(pico-ace
        src/port/audio.c
        src/port/board.c
        src/port/card.c
        src/port/core0.c
        src/port/core1.c
        src/port/diskio.c
        src/port/display.c
        src/port/handoff.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/log.c
        src/port/main.c
        src/port/menu.c
        src/port/park.c
        src/port/sd.c
        src/port/settingsio.c
        src/port/southbridge.c
        src/port/storage.c
        src/port/snapio.c
        src/port/tapeio.c
        src/port/textpage.c
    )
    pico_ace_firmware(pico-ace)
    if (PICO_ACE_BOOT_RAM)
        string(TOUPPER ${PICO_ACE_BOOT_RAM} boot_ram)
        if (NOT boot_ram MATCHES "^(3K|19K|51K)$")
            message(FATAL_ERROR "pico-ace: PICO_ACE_BOOT_RAM is 3k, 19k or 51k, not ${PICO_ACE_BOOT_RAM}")
        endif()
        target_compile_definitions(pico-ace PRIVATE PICO_ACE_BOOT_RAM=ACE_RAM_${boot_ram})
    endif()
    if (PICO_ACE_BOOT_TAPE)
        target_compile_definitions(pico-ace PRIVATE PICO_ACE_BOOT_TAPE="${PICO_ACE_BOOT_TAPE}")
    endif()
    target_compile_definitions(pico-ace PRIVATE PICO_ACE_AUDIO=${PICO_ACE_AUDIO_ON})
    target_link_libraries(pico-ace
        pico_ace_fatfs
        hardware_dma
        hardware_i2c
        hardware_irq
        hardware_pwm
        hardware_spi
        hardware_sync
        pico_multicore
    )

    # M2's bench: the Z80 alone, timed on the forth program and on ZEXDOC
    # when tools/fetch-test-suites.sh has fetched it (design.md §15.2 M2).
    add_executable(pico-ace-bench
        src/port/board.c
        src/port/bench_main.c
    )
    pico_ace_firmware(pico-ace-bench)
    target_link_libraries(pico-ace-bench pico_ace_bench)
    set(PICO_ACE_ZEXDOC ${CMAKE_CURRENT_SOURCE_DIR}/test/suites/zexdoc.com)
    if (EXISTS ${PICO_ACE_ZEXDOC})
        target_sources(pico-ace-bench PRIVATE src/port/bench_zexdoc.S)
        set_source_files_properties(src/port/bench_zexdoc.S PROPERTIES
            OBJECT_DEPENDS ${PICO_ACE_ZEXDOC})
        target_compile_definitions(pico-ace-bench PRIVATE
            PICO_ACE_BENCH_ZEXDOC=1
            PICO_ACE_ZEXDOC_PATH="${PICO_ACE_ZEXDOC}")
    else()
        message(STATUS "pico-ace-bench: no ${PICO_ACE_ZEXDOC}; the forth workload only")
        target_compile_definitions(pico-ace-bench PRIVATE PICO_ACE_BENCH_ZEXDOC=0)
    endif()
endif()
