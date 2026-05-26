#
# frank-cpc — CPC emulator for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.13)

# Platform selection: m1, m2, z0
set(PLATFORM "m2" CACHE STRING "Target platform: m1, m2, z0")
set_property(CACHE PLATFORM PROPERTY STRINGS m1 m2 z0)

if(NOT PLATFORM STREQUAL "m1" AND NOT PLATFORM STREQUAL "m2" AND NOT PLATFORM STREQUAL "z0")
    message(FATAL_ERROR "Unknown PLATFORM '${PLATFORM}' — must be m1, m2, or z0")
endif()

# Z0 (Waveshare RP2350-PiZero) requires a custom board definition
if(PLATFORM STREQUAL "z0")
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Board type" FORCE)
else()
    set(PICO_BOARD "pico2" CACHE STRING "Board type" FORCE)
endif()

# Per-platform compile definition and pin configuration
if(PLATFORM STREQUAL "m1")
    set(PLATFORM_DEF PLATFORM_M1)
    set(BOARD_DEF BOARD_M1)
    set(SD_CS 5)
    set(SD_SCK 2)
    set(SD_MOSI 3)
    set(SD_MISO 4)
    set(HDMI_PIN 6)
    set(VGA_PIN 6)
    set(TV_PIN 6)
elseif(PLATFORM STREQUAL "z0")
    set(PLATFORM_DEF PLATFORM_Z0)
    set(BOARD_DEF BOARD_Z0)
    set(SD_CS 43)
    set(SD_SCK 30)
    set(SD_MOSI 31)
    set(SD_MISO 40)
    set(HDMI_PIN 32)
    set(VGA_PIN 32)
else()
    # m2
    set(PLATFORM_DEF PLATFORM_M2)
    set(BOARD_DEF BOARD_M2)
    set(SD_CS 5)
    set(SD_SCK 6)
    set(SD_MOSI 7)
    set(SD_MISO 4)
    set(HDMI_PIN 12)
    set(VGA_PIN 12)
    set(TV_PIN 12)
endif()

set(CPU_SPEED  "504" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "100" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash target max frequency in MHz")

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 300)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

# Video output selection
set(HDMI_DRIVER "HDMI_PIO" CACHE STRING "HDMI driver: HDMI_PIO (I2S audio) or HDMI_PIO_AUDIO (HDMI audio) or VGA_HSTX (HSTX VGA + I2S) or COMPOSITE (composite TV + I2S)")
set_property(CACHE HDMI_DRIVER PROPERTY STRINGS "HDMI_PIO" "HDMI_PIO_AUDIO" "VGA_HSTX" "COMPOSITE")

# Validate video driver vs platform constraints
if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO" AND NOT PLATFORM STREQUAL "m2")
    message(FATAL_ERROR "HDMI_PIO_AUDIO is only supported on the M2 platform (got '${PLATFORM}')")
endif()
if(HDMI_DRIVER STREQUAL "VGA_HSTX" AND NOT PLATFORM STREQUAL "m2")
    message(FATAL_ERROR "VGA_HSTX is only supported on the M2 platform (got '${PLATFORM}')")
endif()
if(HDMI_DRIVER STREQUAL "COMPOSITE" AND PLATFORM STREQUAL "z0")
    message(FATAL_ERROR "Composite TV is not supported on the Z0 platform")
endif()

# Composite-TV needs ~378 MHz: the TV scanline callback fires via a
# flash-resident alarm-pool dispatcher at 30 kHz, and at 252 MHz the
# dispatch overhead alone consumes most of each ~32 µs window.
if(HDMI_DRIVER STREQUAL "COMPOSITE" AND CPU_SPEED LESS 300)
    message(STATUS "Overriding CPU_SPEED to 378 MHz for composite TV")
    set(CPU_SPEED "378" CACHE STRING "" FORCE)
    # Recompute voltage for the new speed
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
endif()

message(STATUS "frank-cpc  Platform: ${PLATFORM}  CPU: ${CPU_SPEED} MHz  PSRAM: ${PSRAM_SPEED} MHz  Flash: ${FLASH_SPEED} MHz  Voltage: ${CPU_VOLTAGE}  HDMI: ${HDMI_DRIVER}")

include(pico_sdk_import.cmake)

project(frank-cpc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" FRANK_CPC_VERSION_RAW)
string(STRIP "${FRANK_CPC_VERSION_RAW}" FRANK_CPC_VERSION_RAW)
string(REPLACE " " "." FRANK_CPC_VERSION_STR "${FRANK_CPC_VERSION_RAW}")

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2)
add_subdirectory(drivers/nespad)

# Propagate platform define to sub-library targets that include board_config.h
target_compile_definitions(ps2_driver PRIVATE ${PLATFORM_DEF}=1)

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    # --- HDMI_PIO_AUDIO: frank-hdmi-sound library (HDMI-embedded audio) ---
    # TMDS bit clock = sys_clock / CLKDIV.  Need 252 MHz TMDS.
    #   CPU ≤ 252 MHz → CLKDIV=1;  CPU > 252 MHz (e.g. 504) → CLKDIV=2.
    if(CPU_SPEED GREATER 252)
        set(FRANK_HDMI_SM_CLKDIV "2" CACHE STRING "" FORCE)
    else()
        set(FRANK_HDMI_SM_CLKDIV "1" CACHE STRING "" FORCE)
    endif()
    add_subdirectory(frank-hdmi-sound/src)

    add_library(drivers
        drivers/HDMI_audio.c
        drivers/HDMI_vga.c
        drivers/test_pins.c
        drivers/crash_handler.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        HDMI_PIO_AUDIO=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        frank_hdmi_sound
    )

elseif(HDMI_DRIVER STREQUAL "VGA_HSTX")
    # --- VGA_HSTX: DispHSTX library (VGA via HSTX) + I2S audio ---
    add_subdirectory(drivers/disphstx)

    add_library(drivers
        drivers/HDMI_vga_hstx.c
        drivers/crash_handler.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        VGA_HSTX=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        disphstx
    )

elseif(HDMI_DRIVER STREQUAL "COMPOSITE")
    # --- COMPOSITE: Software composite TV (PAL/NTSC) + I2S audio ---
    add_library(tv_driver STATIC
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
    )
    target_include_directories(tv_driver PUBLIC drivers drivers/tv)
    set(_tv_include_flags
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_rename.h"
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_mem.h"
    )
    string(JOIN " " _tv_include_flags_str ${_tv_include_flags})
    set_source_files_properties(
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
        PROPERTIES
        COMPILE_FLAGS "${_tv_include_flags_str}"
    )
    target_compile_options(tv_driver PRIVATE
        -Wno-error
        -Wno-incompatible-pointer-types
        -Wno-all
        -fno-tree-loop-distribute-patterns
        -fno-builtin-memset
        -fno-builtin-memcpy
    )
    target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=${TV_PIN} ${PLATFORM_DEF}=1)
    target_link_libraries(tv_driver
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_clocks
    )

    add_library(drivers
        drivers/HDMI_tv.c
        drivers/crash_handler.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        VIDEO_COMPOSITE=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
        tv_driver
    )

else()
    # --- HDMI_PIO: original PIO HDMI driver (I2S audio) ---
    add_library(drivers
        drivers/HDMI.c
        drivers/hdmi_scanline.S
        drivers/HDMI_vga.c
        drivers/test_pins.c
        drivers/crash_handler.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/pwm_audio/pwm_audio.c
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        ${BOARD_DEF}
        ${PLATFORM_DEF}=1
        HDMI_PIO=1
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
    target_link_libraries(drivers
        pico_stdlib
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_gpio
        hardware_clocks
        hardware_irq
        hardware_vreg
        hardware_exception
        hardware_watchdog
    )
endif()

set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1")

set(CAP32_ENGINE
    src/cap32/z80.cpp
    src/cap32/cap32.cpp
    src/cap32/crtc.cpp
    src/cap32/psg.cpp
    src/cap32/fdc.cpp
    src/cap32/disk.cpp
    src/cap32/tape.cpp
    src/cpc_adapter.cpp
    src/cpc_cartridge.cpp
    src/cpc_ipf.cpp
)

# capsimg library (CAPS/IPF disk image support)
set(CAPSIMG_SOURCES
    src/capsimg/Core/BitBuffer.cpp
    src/capsimg/Core/BaseFile.cpp
    src/capsimg/Core/CRC.cpp
    src/capsimg/Core/DiskFile.cpp
    src/capsimg/Core/MemoryFile.cpp
    src/capsimg/Codec/CTRawCodec.cpp
    src/capsimg/Codec/CTRawCodecDecompressor.cpp
    src/capsimg/Codec/DiskEncoding.cpp
    src/capsimg/CAPSImg/CAPSImg.cpp
    src/capsimg/CAPSImg/CapsAPI.cpp
    src/capsimg/CAPSImg/CapsFile.cpp
    src/capsimg/CAPSImg/CapsFDCEmulator.cpp
    src/capsimg/CAPSImg/CapsFormatMFM.cpp
    src/capsimg/CAPSImg/CapsImage.cpp
    src/capsimg/CAPSImg/CapsImageStd.cpp
    src/capsimg/CAPSImg/CapsLoader.cpp
    src/capsimg/CAPSImg/DiskImage.cpp
    src/capsimg/CAPSImg/DiskImageFactory.cpp
    src/capsimg/CAPSImg/StreamCueImage.cpp
    src/capsimg/CAPSImg/StreamImage.cpp
    src/capsimg/CAPSImg/stdafx.cpp
)

set(CPC_PLATFORM
    src/tape_rec.c
    src/tape_gpio.c
)

set(PICO_PORT
    src/Pico/main.c
    src/Pico/platform.c
    src/Pico/cpc_settings.c
    src/Pico/cpc_loader.c
    src/Pico/cpc_tape_loader.c
    src/Pico/cpc_ui.c
    src/Pico/cpc_autotype.c
    src/Pico/ui_draw.c
    src/Pico/ui_font.c
)

set(CPC_STUBS
    src/printer.c
    src/dialogs.c
)

add_executable(frank-cpc
    ${CAP32_ENGINE}
    ${CAPSIMG_SOURCES}
    ${CPC_PLATFORM}
    ${PICO_PORT}
    ${CPC_STUBS}
)

target_include_directories(frank-cpc PRIVATE
    src
    src/cap32
    src/Pico
    src/capsimg/Core
    src/capsimg/CAPSImg
    src/capsimg/Codec
    src/capsimg/LibIPF
    src/capsimg/Device
    drivers
    drivers/pwm_audio
    drivers/fatfs
    drivers/sdcard
    drivers/ps2
    drivers/nespad
)

target_compile_definitions(frank-cpc PRIVATE
    ${BOARD_DEF}
    ${PLATFORM_DEF}=1
    PICO_BUILD=1
    PICO_ON_DEVICE=1
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    LSB_FIRST=1
    FRANK_CPC_VERSION=\"${FRANK_CPC_VERSION_STR}\"
    SDCARD_PIN_SPI0_CS=${SD_CS}
    SDCARD_PIN_SPI0_SCK=${SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${SD_MISO}
)

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    target_compile_definitions(frank-cpc PRIVATE HDMI_PIO_AUDIO=1)
    target_include_directories(frank-cpc PRIVATE frank-hdmi-sound/src)
elseif(HDMI_DRIVER STREQUAL "VGA_HSTX")
    target_compile_definitions(frank-cpc PRIVATE VGA_HSTX=1)
elseif(HDMI_DRIVER STREQUAL "COMPOSITE")
    target_compile_definitions(frank-cpc PRIVATE VIDEO_COMPOSITE=1)
    target_link_libraries(frank-cpc tv_driver)
endif()

# Z0: SD card pins are above GPIO 28 — requires SPI1 instead of SPI0
if(PLATFORM STREQUAL "z0")
    target_compile_definitions(frank-cpc PRIVATE SDCARD_SPI_BUS=spi1)
    target_link_libraries(frank-cpc hardware_spi)
endif()

target_compile_options(frank-cpc PRIVATE
    -O3
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-braces
    -Wno-implicit-fallthrough
    -Wno-pointer-sign
    -Wno-incompatible-pointer-types
)

# Hot engine files already get -O3 from global setting
set_source_files_properties(
    src/cap32/z80.cpp
    src/cap32/crtc.cpp
    src/cap32/psg.cpp
    src/cap32/cap32.cpp
    src/cpc_adapter.cpp
    PROPERTIES COMPILE_FLAGS "-O3"
)

# capsimg library: suppress warnings from legacy code
set_source_files_properties(${CAPSIMG_SOURCES}
    PROPERTIES COMPILE_FLAGS
    "-Wno-old-style-cast -Wno-zero-as-null-pointer-constant -Wno-parentheses -Wno-reorder -Wno-class-memaccess -Wno-narrowing -Wno-missing-field-initializers -Wno-delete-non-virtual-dtor"
)

target_link_libraries(frank-cpc
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_vreg
    hardware_clocks
    sdcard
    fatfs
    drivers
    ps2_driver
    nespad
)

pico_enable_stdio_uart(frank-cpc 0)
pico_enable_stdio_usb(frank-cpc 1)

pico_add_extra_outputs(frank-cpc)
