cmake_minimum_required(VERSION 3.13)

# WeAct Studio Core2350B (RP2350B, 16 MB flash, PSRAM-capable)
set(PICO_BOARD weact_studio_rp2350b_core)

include(pico_sdk_import.cmake)

project(emerald_hwtest C CXX ASM)

pico_sdk_init()

# Phase-1 hardware self-test: boots, brings up stdio, and unit-tests the RP2350
# BIOS reimplementations (rp2350/bios.c) on real silicon.
add_executable(emerald_hwtest
    test_main.c
    ${CMAKE_CURRENT_LIST_DIR}/../bios.c
)

# bios.c is a game translation unit: it needs the pokeemerald include tree and
# the MODERN/RP2350 defines (which select the SRAM memory map). test_main.c uses
# only SDK headers, so the two TUs never clash.
# Use -iquote (not -I) like the real game build, so the game's include/strings.h
# etc. only satisfy "" includes and never hijack libc <strings.h>/<string.h>.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../bios.c PROPERTIES
    COMPILE_DEFINITIONS "MODERN=1;RP2350=1"
    COMPILE_OPTIONS "-iquote;${CMAKE_CURRENT_LIST_DIR}/../../include"
)

target_link_libraries(emerald_hwtest pico_stdlib)

pico_enable_stdio_usb(emerald_hwtest 1)
pico_enable_stdio_uart(emerald_hwtest 1)

pico_add_extra_outputs(emerald_hwtest)

# ---- HSTX -> DVI/HDMI bring-up test ----
# Standalone Phase-2 display bring-up: 640x480@60 colour bars over HSTX, to
# validate the wired HSTX-to-HDMI breakout on real silicon before the PPU.
add_executable(hstx_test hstx_test.c)
target_link_libraries(hstx_test
    pico_stdlib
    hardware_dma
)
pico_enable_stdio_usb(hstx_test 1)
pico_enable_stdio_uart(hstx_test 0)
pico_add_extra_outputs(hstx_test)

# ---- PSRAM bring-up test ----
# Inits the APS6404 PSRAM (QSPI CS1 / GPIO 47) and pattern-tests it on silicon.
add_executable(psram_test psram_test.c ${CMAKE_CURRENT_LIST_DIR}/../psram.c)
target_link_libraries(psram_test pico_stdlib hardware_clocks)
pico_enable_stdio_usb(psram_test 1)
pico_enable_stdio_uart(psram_test 0)
pico_add_extra_outputs(psram_test)

# ---- I2S audio bring-up test (PCM5102A on GP20/21/22) ----
# Plays a 440 Hz sine to validate the I2S wiring on silicon before m4a revival.
add_executable(i2s_test i2s_test.c i2s_audio.c)
pico_generate_pio_header(i2s_test ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio)
target_link_libraries(i2s_test pico_stdlib hardware_pio hardware_dma)
pico_enable_stdio_usb(i2s_test 1)
pico_enable_stdio_uart(i2s_test 0)
pico_add_extra_outputs(i2s_test)

# ---- Display driver test (SRAM framebuffer) ----
add_executable(display_test display_test.c ${CMAKE_CURRENT_LIST_DIR}/../hstx_display.c)
target_link_libraries(display_test pico_stdlib hardware_dma hardware_clocks hardware_vreg)
pico_enable_stdio_usb(display_test 1)
pico_enable_stdio_uart(display_test 0)
pico_add_extra_outputs(display_test)

# ---- PPU + display integration test (renders a captured snapshot) ----
add_executable(ppu_display_test ppu_display_test.c
    ${CMAKE_CURRENT_LIST_DIR}/../hstx_display.c
    ${CMAKE_CURRENT_LIST_DIR}/../ppu.c)
target_link_libraries(ppu_display_test pico_stdlib hardware_dma hardware_clocks hardware_vreg)
pico_enable_stdio_usb(ppu_display_test 1)
pico_enable_stdio_uart(ppu_display_test 0)
pico_add_extra_outputs(ppu_display_test)

# ---- Full game target ----
# Links the prebuilt game archive (built by rp2350/build_objs.sh) with the SDK
# boot/runtime and the custom memory map. Whole-archive so all game data is
# included (matches the GBA build; nothing is reachability-trimmed).
add_executable(emerald
    game_main.c
    flash_save.c
    audio.c
    i2s_audio.c
    ${CMAKE_CURRENT_LIST_DIR}/../ppu.c
    ${CMAKE_CURRENT_LIST_DIR}/../hstx_display.c
)
pico_generate_pio_header(emerald ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio)

pico_set_linker_script(emerald ${CMAKE_CURRENT_LIST_DIR}/memmap_emerald.ld)

# The SDK RAM region is full (framebuffer + layer scratch), so the PPU's
# palette LUTs go into EWRAM's slack -- only valid with memmap_emerald.ld,
# hence per-target (ppu_display_test keeps them in ordinary .bss).
# PICO_CORE1_STACK_SIZE=0 drops pico_multicore's default .stack1 (it would
# overflow the shrunken SCRATCH_X); core 1 gets an explicit stack in EWRAM via
# multicore_launch_core1_with_stack.
# AUDIO_BUFS_IN_IWRAM parks the I2S DMA buffers + the audio ring in IWRAM slack
# (.iwram_top); EWRAM and the SDK RAM region are both full. To hear a test tone
# before the m4a mixer is ported, add AUDIO_TEST_TONE=1 here.
target_compile_definitions(emerald PRIVATE
    PPU_LUTS_IN_EWRAM=1
    DISP_BLACK_IN_EWRAM=1
    PPU_PROFILE=1
    AUDIO_BUFS_IN_IWRAM=1
    PICO_CORE1_STACK_SIZE=0)

target_link_libraries(emerald
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_flash
    hardware_xip_cache
    hardware_dma
    hardware_clocks
    hardware_vreg
    hardware_pio
    -Wl,--whole-archive ${CMAKE_CURRENT_LIST_DIR}/../build/libpokeemerald.a -Wl,--no-whole-archive
)

pico_enable_stdio_usb(emerald 1)
# stdio-UART OFF: UART0's default pins (GP12/13) belong to HSTX, and stdio
# writes block at the 115200 char rate -- it tripled every print's cost and
# turned the 5 s stats burst into an ~100 ms frame hitch.
pico_enable_stdio_uart(emerald 0)

pico_add_extra_outputs(emerald)
