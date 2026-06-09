add_library(disphstx STATIC
    ${CMAKE_CURRENT_LIST_DIR}/disphstx.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_picolibsk.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga_render.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga_m33.S
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi_render.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi_m33.S
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_simple.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_format.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_time.c
)

target_compile_definitions(disphstx PUBLIC
    USE_DISPHSTX=1
    DISPHSTX_PICOSDK=1
    # DVI (HDMI) render core is unused on the VGA_HSTX path (HDMI uses the PIO
    # driver, not DispHSTX). Disabling it frees the DVI scanout core from RAM.
    DISPHSTX_USE_DVI=0
    DISPHSTX_USE_VGA=1
    USE_DRAWCAN=0
    USE_TEXT=0
    USE_RAND=0
    DISPHSTX_DVI_PINOUT=0
    # frank-cpc only ever uses DispVMode360x240x8_Fast (FORMAT_8 / RGB332).
    # Disable every other pixel format to drop their palette tables and
    # per-format render code from RAM.
    DISPHSTX_USE_FORMAT_1=0
    DISPHSTX_USE_FORMAT_2=0
    DISPHSTX_USE_FORMAT_3=0
    DISPHSTX_USE_FORMAT_4=0
    DISPHSTX_USE_FORMAT_6=0
    DISPHSTX_USE_FORMAT_8=1
    DISPHSTX_USE_FORMAT_12=0
    DISPHSTX_USE_FORMAT_15=0
    DISPHSTX_USE_FORMAT_16=0
    DISPHSTX_USE_FORMAT_1_PAL=0
    DISPHSTX_USE_FORMAT_2_PAL=0
    DISPHSTX_USE_FORMAT_3_PAL=0
    DISPHSTX_USE_FORMAT_4_PAL=0
    DISPHSTX_USE_FORMAT_6_PAL=0
    DISPHSTX_USE_FORMAT_8_PAL=0
    DISPHSTX_USE_FORMAT_COL=0
    DISPHSTX_USE_FORMAT_MTEXT=0
    DISPHSTX_USE_FORMAT_ATEXT=0
    DISPHSTX_USE_FORMAT_TILE4_8=0
    DISPHSTX_USE_FORMAT_TILE8_8=0
    DISPHSTX_USE_FORMAT_TILE16_8=0
    DISPHSTX_USE_FORMAT_TILE32_8=0
    DISPHSTX_USE_FORMAT_TILE4_8_PAL=0
    DISPHSTX_USE_FORMAT_TILE8_8_PAL=0
    DISPHSTX_USE_FORMAT_TILE16_8_PAL=0
    DISPHSTX_USE_FORMAT_TILE32_8_PAL=0
    DISPHSTX_USE_FORMAT_HSTX_15=0
    DISPHSTX_USE_FORMAT_HSTX_16=0
    DISPHSTX_USE_FORMAT_PAT_8=0
    DISPHSTX_USE_FORMAT_PAT_8_PAL=0
    DISPHSTX_USE_FORMAT_RLE8=0
    DISPHSTX_USE_FORMAT_RLE8_PAL=0
    DISPHSTX_USE_FORMAT_ATTR1_PAL=0
    DISPHSTX_USE_FORMAT_ATTR2_PAL=0
    DISPHSTX_USE_FORMAT_ATTR3_PAL=0
    DISPHSTX_USE_FORMAT_ATTR4_PAL=0
    DISPHSTX_USE_FORMAT_ATTR5_PAL=0
    DISPHSTX_USE_FORMAT_ATTR6_PAL=0
    DISPHSTX_USE_FORMAT_ATTR7_PAL=0
    DISPHSTX_USE_FORMAT_ATTR8_PAL=0
    # Render-line buffer is sized by WIDTHMAX; we only need the 720-wide output
    # line. Single videomode strip/slot in use, so cap those arrays too.
    DISPHSTX_WIDTHMAX=736
    DISPHSTX_STRIP_MAX=1
    DISPHSTX_SLOT_MAX=1
)

target_include_directories(disphstx PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(disphstx PUBLIC
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
    hardware_dma
    hardware_irq
    hardware_pll
    hardware_vreg
)

target_compile_options(disphstx PRIVATE
    -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter
    -Wno-unused-but-set-variable -Wno-implicit-function-declaration
    -Wno-sign-compare -Wno-incompatible-pointer-types
    -Wno-int-conversion -Wno-pointer-sign
)
