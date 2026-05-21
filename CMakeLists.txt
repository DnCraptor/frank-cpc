#
# frank-cpc — CPC emulator for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.13)

set(PLATFORM "m2" CACHE STRING "Target platform")
if(NOT PLATFORM STREQUAL "m2")
    message(FATAL_ERROR "frank-cpc supports only PLATFORM=m2")
endif()

set(PICO_BOARD "pico2" CACHE STRING "Pico board type")
set(CPU_SPEED  "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash target max frequency in MHz")

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 300)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

set(HDMI_DRIVER "HDMI_PIO" CACHE STRING "HDMI driver: HDMI_PIO (I2S audio) or HDMI_PIO_AUDIO (HDMI audio) or VGA_HSTX (HSTX VGA + I2S)")
set_property(CACHE HDMI_DRIVER PROPERTY STRINGS "HDMI_PIO" "HDMI_PIO_AUDIO" "VGA_HSTX")

message(STATUS "frank-cpc  CPU: ${CPU_SPEED} MHz  PSRAM: ${PSRAM_SPEED} MHz  Flash: ${FLASH_SPEED} MHz  Voltage: ${CPU_VOLTAGE}  HDMI: ${HDMI_DRIVER}")

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

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    # --- HDMI_PIO_AUDIO: frank-hdmi-sound library (HDMI-embedded audio) ---
    set(FRANK_HDMI_SM_CLKDIV "1" CACHE STRING "" FORCE)
    add_subdirectory(frank-hdmi-sound/src)

    add_library(drivers
        drivers/HDMI_audio.c
        drivers/HDMI_vga.c
        drivers/test_pins.c
        drivers/crash_handler.c
        drivers/audio.c
        drivers/audio_i2s_opt.S
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers src)
    target_compile_definitions(drivers PUBLIC
        BOARD_M2
        PLATFORM_M2=1
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
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers src)
    target_compile_definitions(drivers PUBLIC
        BOARD_M2
        PLATFORM_M2=1
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
        drivers/psram_init.c
        drivers/psram_allocator.c
        drivers/dlmalloc.c
        drivers/uart_logging.c
    )

    target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
    target_compile_definitions(drivers PUBLIC
        BOARD_M2
        PLATFORM_M2=1
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

set(CPC_CORE
    src/Z80/Z80.c
    src/Z80/z80_arm.S
    src/screenpos.c
    src/disc.c
    src/io.c
    src/mem.c
)

set(CPC_ADAPTED
    src/cpc.c
    src/screen.c
    src/colors.c
    src/keyboard.c
    src/aysound.c
)

set(PICO_PORT
    src/Pico/main.c
    src/Pico/platform.c
    src/Pico/cpc_settings.c
    src/Pico/cpc_loader.c
    src/Pico/cpc_ui.c
    src/Pico/cpc_autotype.c
    src/Pico/ui_draw.c
    src/Pico/ui_font.c
)

set(CPC_STUBS
    src/dialogs.c
    src/printer.c
)

add_executable(frank-cpc
    ${CPC_CORE}
    ${CPC_ADAPTED}
    ${PICO_PORT}
    ${CPC_STUBS}
)

target_include_directories(frank-cpc PRIVATE
    src
    src/Z80
    src/Pico
    drivers
    drivers/fatfs
    drivers/sdcard
    drivers/ps2
)

target_compile_definitions(frank-cpc PRIVATE
    BOARD_M2
    PLATFORM_M2=1
    PICO_BUILD=1
    PICO_ON_DEVICE=1
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    LSB_FIRST=1
    FRANK_CPC_VERSION=\"${FRANK_CPC_VERSION_STR}\"
    SDCARD_PIN_SPI0_CS=5
    SDCARD_PIN_SPI0_SCK=6
    SDCARD_PIN_SPI0_MOSI=7
    SDCARD_PIN_SPI0_MISO=4
)

if(HDMI_DRIVER STREQUAL "HDMI_PIO_AUDIO")
    target_compile_definitions(frank-cpc PRIVATE HDMI_PIO_AUDIO=1)
    target_include_directories(frank-cpc PRIVATE frank-hdmi-sound/src)
elseif(HDMI_DRIVER STREQUAL "VGA_HSTX")
    target_compile_definitions(frank-cpc PRIVATE VGA_HSTX=1)
endif()

target_compile_options(frank-cpc PRIVATE
    -O2
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
)

pico_enable_stdio_uart(frank-cpc 0)
pico_enable_stdio_usb(frank-cpc 1)

pico_add_extra_outputs(frank-cpc)
