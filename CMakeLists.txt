# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Target board: Waveshare RP2350-PiZero. Use Pico-Toolset's shared
# boards/waveshare_rp2350_pizero.h (this project's own copy, and TOM6809's
# identical copy, both moved there -- see third_party/pico-toolset's README)
# ahead of the SDK's, since it adds PICO_PSRAM_CS_PIN and
# PICO_DEFAULT_PIO_USB_DP_PIN the SDK's version lacks.
# PICO_BOARD must be defined BEFORE pico_sdk_import.cmake: its
# pico_pre_load_platform runs at include time and decides the toolchain/platform
# (rp2350 vs rp2040) from it.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
endif()
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico-toolset/boards CACHE STRING "" FORCE)

# Output driver: "lcd" (default, external Waveshare ILI9486 SPI panel) or
# "hdmi" (onboard DVI/HDMI TMDS connector, via Pico-Toolset's
# pico_toolset_dvi_hdmi -- see docs/HDMI_PLAN.md). A single enum-valued cache
# variable rather than two independent booleans (TOM6809's
# WAVESHARE_HDMI_SUPPORT/WAVESHARE_LCD_SUPPORT pattern): "both at once" is
# then structurally impossible, so no separate FATAL_ERROR guard is needed
# beyond validating the string itself, below.
set(PICODOOM_VIDEO_OUTPUT "lcd" CACHE STRING "Output driver: lcd | hdmi")
set_property(CACHE PICODOOM_VIDEO_OUTPUT PROPERTY STRINGS lcd hdmi)
if(NOT PICODOOM_VIDEO_OUTPUT STREQUAL "lcd" AND NOT PICODOOM_VIDEO_OUTPUT STREQUAL "hdmi")
    message(FATAL_ERROR "PICODOOM_VIDEO_OUTPUT must be 'lcd' or 'hdmi', got '${PICODOOM_VIDEO_OUTPUT}'")
endif()

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(PicoDoom C CXX ASM)
set(CMAKE_C_STANDARD 11)
# 20, not 17: Pico-Toolset's pico_toolset::Ili9486 (src/i_video_ili9486.cpp)
# uses std::span, C++20-only.
set(CMAKE_CXX_STANDARD 20)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Pico-Toolset (submodule, third_party/pico-toolset) provides every
# non-engine driver this project uses: pico_toolset_ili9486 (LCD) or
# pico_toolset_dvi_hdmi (HDMI, see PICODOOM_VIDEO_OUTPUT above), depending on
# the selected output driver; pico_toolset_usb_hid (PIO-USB keyboard/mouse
# host, PICO_DEFAULT_PIO_USB_DP_PIN/GPIO28-29 -- RP2350's own USB peripheral
# is native-device-only and already committed to stdio_usb's serial
# console), pico_toolset_sdcard (uSD/FatFs + its opt-in POSIX/stdio syscall
# shim), pico_toolset_psram, and pico_toolset_fault_handler.
# SSD1306/XPT2046/I2S_AUDIO/RESET_BUTTONS/SCREEN are unused by this project
# -- off to skip building them, regardless of output driver.
set(PICO_TOOLSET_BUILD_SSD1306 OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_XPT2046 OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_I2S_AUDIO OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_RESET_BUTTONS OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_SCREEN OFF CACHE BOOL "" FORCE)
if(PICODOOM_VIDEO_OUTPUT STREQUAL "hdmi")
    set(PICO_TOOLSET_BUILD_ILI9486 OFF CACHE BOOL "" FORCE)
    set(PICO_TOOLSET_BUILD_DVI_HDMI ON CACHE BOOL "" FORCE)
    # HDMI data-island digital audio -- on from Phase B (SFX) onward, see
    # docs/HDMI_PLAN.md's phased rollout and src/i_sound_dvi.cpp.
    set(PICO_TOOLSET_DVI_HDMI_AUDIO ON CACHE BOOL "" FORCE)
else()
    set(PICO_TOOLSET_BUILD_ILI9486 ON CACHE BOOL "" FORCE)
    set(PICO_TOOLSET_BUILD_DVI_HDMI OFF CACHE BOOL "" FORCE)
    set(PICO_TOOLSET_DVI_HDMI_AUDIO OFF CACHE BOOL "" FORCE)
endif()
# Doom's WAD loader (doom/w_wad.c) seeks around inside files via libc stdio,
# so this project needs the sdcard component's POSIX syscall shim, not just
# its whole-file-into-memory list_files()/read_file() API -- see
# src/sd_stdio.cpp.
set(PICO_TOOLSET_SDCARD_STDIO ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/pico-toolset)

SET(DOOMSRC
doom/doomdef.c
doom/doomstat.c
doom/dstrings.c
doom/i_system.c
#doom/i_sound.c     # Linux OSS — not linkable on Pico
#doom/i_video.c     # X11 — not linkable on Pico
#doom/i_net.c       # Linux IPX — not linkable on Pico
#doom/i_video_pimoroni.c   # also X11 (misnomer)
#doom/i_video_null.c       # headless stub, replaced by src/i_video_ili9486.cpp
#doom/i_sound_null.c       # headless stub -- added below via PICODOOM_SOUND_SRC instead
doom/i_net_null.c
doom/tables.c
doom/f_finale.c
doom/f_wipe.c
doom/d_main.c
doom/d_net.c
doom/d_items.c
doom/g_game.c
doom/m_menu.c
doom/m_misc.c
doom/m_argv.c
doom/m_bbox.c
doom/m_fixed.c
doom/m_swap.c
doom/m_cheat.c
doom/m_random.c
doom/am_map.c
doom/p_ceilng.c
doom/p_doors.c
doom/p_enemy.c
doom/p_floor.c
doom/p_inter.c
doom/p_lights.c
doom/p_map.c
doom/p_maputl.c
doom/p_plats.c
doom/p_pspr.c
doom/p_setup.c
doom/p_sight.c
doom/p_spec.c
doom/p_switch.c
doom/p_mobj.c
doom/p_telept.c
doom/p_tick.c
doom/p_saveg.c
doom/p_user.c
doom/r_bsp.c
doom/r_data.c
doom/r_draw.c
doom/r_main.c
doom/r_plane.c
doom/r_segs.c
doom/r_sky.c
doom/r_things.c
doom/w_wad.c
doom/wi_stuff.c
doom/v_video.c
doom/st_lib.c
doom/st_stuff.c
doom/hu_stuff.c
doom/hu_lib.c
doom/s_sound.c
doom/z_zone.c
doom/info.c
doom/sounds.c
)

# The 1993-era engine sources rely on pre-C99 features (implicit int,
# C89 for-scope). Keep them on gnu90 except d_main.c, which includes
# pico-sdk headers that need C11.
#
# -w on every DOOMSRC file (added for Phase 3): linking tinyusb_host below
# pulls in TinyUSB's own family_support.cmake, which sets -Wall -Wextra as
# PUBLIC compile options on that target -- and PUBLIC means every consumer
# (this whole PicoDoom target, engine sources included) inherits them,
# whether or not it asked to. That's a reasonable default for OUR code
# (src/*.cpp -- worth catching real mistakes in, especially freshly-written
# Phase 3 code), but doom/ is untouched 1993 id Software source (see
# AGENTS.md) full of code that predates -Wall by decades: unused params in
# stub-shaped functions, unused rcsid[] string constants, sign-compare in
# straightforward loops. Not our code to restyle. -w (after -Wall -Wextra
# on the command line, and GCC treats it as a blanket override regardless
# of position) restores this project's own "zero warnings" bar to mean
# what it always meant: clean on the code we actually maintain.
#
# -fno-strict-aliasing on every DOOMSRC file (2026-09, added while chasing
# the demo/gameplay freeze investigated in docs/PLAN.md): a freeze that
# reproduced in Release/RelWithDebInfo but not Debug -- i.e. only with
# GCC's optimizer active -- looked at the time like the textbook signature
# of a strict-aliasing violation (-fstrict-aliasing only takes effect from
# -O2, and this 1993 codebase reinterprets raw WAD lump bytes as
# patch_t/mobj_t/etc. through pointer casts constantly, exactly the
# pattern that violates it). Turned out not to be the actual cause -- the
# real bug was a type-punning read in st_stuff.c's w_arms[] widget,
# unrelated to aliasing -- but kept anyway: it's still the right, standard
# thing to build 1993-vintage C against with a modern optimizing compiler,
# regardless of whether it explained this specific freeze.
foreach(f ${DOOMSRC})
    if(NOT f MATCHES "d_main\\.c$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-std=gnu90;-w;-fno-strict-aliasing")
    else()
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")
    endif()
endforeach()

# Video/sound source swap, driven by PICODOOM_VIDEO_OUTPUT (see above).
set(PICODOOM_VIDEO_SRC src/i_video_ili9486.cpp)
set(PICODOOM_SOUND_SRC doom/i_sound_null.c)
if(PICODOOM_VIDEO_OUTPUT STREQUAL "hdmi")
    set(PICODOOM_VIDEO_SRC src/i_video_dvi.cpp)
    # Phase B (SFX) of docs/HDMI_PLAN.md's rollout -- see src/i_sound_dvi.cpp.
    set(PICODOOM_SOUND_SRC src/i_sound_dvi.cpp)
endif()

# rest of your project
add_executable(PicoDoom
src/PicoDoom.cpp
src/sd_stdio.cpp
${PICODOOM_VIDEO_SRC}
src/i_input_usbhid.cpp
${PICODOOM_SOUND_SRC}
${DOOMSRC}
)

# LTO tried (2026-09 performance work) and reverted: enabling
# INTERPROCEDURAL_OPTIMIZATION on this target fails at link time with a
# GCC/assembler "offset out of range" error during LTO code generation,
# unfixed by either -mlong-calls or -flto-partition=none (same failure,
# same translation unit, both times). Likely aggravated by how much ends
# up in this one LTO unit: pico_toolset_usb_hid brings in TinyUSB/
# Pico-PIO-USB/FatFs as direct sources of the PicoDoom target itself (see
# the target_link_libraries comment below), not a separate static library,
# so LTO here means combining thousands of USB-stack functions with all of
# Doom's C code in one pass -- not just Doom's own hot loops as intended.
# Motivation was doom/r_draw.c's #ifdef PICO reload-caching blocks (GCC,
# compiling one .c file at a time, couldn't prove hot loops' *dest stores
# didn't alias file-scope globals like dc_source/dc_colormap); LTO might
# have caught that class of thing elsewhere too, but isn't worth chasing
# this toolchain limitation to find out.

# pico_toolset_sdcard (uSD/FatFs + POSIX stdio shim), pico_toolset_psram
# (QMI PSRAM), pico_toolset_fault_handler (hard-fault -> next-boot report),
# pico_toolset_ili9486 (LCD over SPI1, DMA-fed pixel pushes -- brings in
# hardware_spi/hardware_dma), pico_toolset_usb_hid (PIO-USB keyboard/mouse
# host -- brings in pico_pio_usb/tinyusb_host, hcd_pio_usb.c as an INTERFACE
# source of pico_pio_usb, and the PICO_STDIO_USB_* compile definitions
# below), pico_multicore (src/i_input_usbhid.cpp launches core1 itself --
# see that file for why), hardware_watchdog (doom/i_system.c's I_Quit ->
# watchdog_reboot()), on top of pico_stdlib.
target_link_libraries(PicoDoom
    pico_stdlib
    # -u,_stat: pico_toolset_sdcard's sdcard_stdio.cpp.o defines
    # _open/_read/_write/_lseek/_fstat/_stat/_isatty together in one object
    # file, overriding newlib's weak syscalls. The linker only pulls an
    # object out of a static archive for a symbol already known to be
    # undefined at the point it scans that archive -- doom/w_wad.c's
    # fopen() calls create an early need for _open (pulling the object in),
    # but newlib's own libg.a only discovers ITS transitive need for _stat
    # (fopen -> ... -> _stat_r -> _stat) afterwards, once the archive has
    # already been scanned and won't be revisited: "undefined reference to
    # _stat" even though it's right there in the archive. Forcing _stat
    # "undefined" from the start (rather than -Wl,--whole-archive, which
    # pulls in every object -- colliding with PicoDoom's own copies of
    # pio.c/spi.c that pico_toolset_sdcard also happens to link) makes the
    # linker need it, and therefore pull sdcard_stdio.cpp.o in, on the first
    # and only pass over that archive.
    -Wl,-u,_stat
    pico_toolset_sdcard
    pico_toolset_psram
    pico_toolset_fault_handler
    $<$<STREQUAL:${PICODOOM_VIDEO_OUTPUT},lcd>:pico_toolset_ili9486>
    $<$<STREQUAL:${PICODOOM_VIDEO_OUTPUT},hdmi>:pico_toolset_dvi_hdmi>
    pico_toolset_usb_hid
    pico_multicore
    hardware_watchdog
)

target_include_directories(PicoDoom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/doom
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(PicoDoom PRIVATE PICO)
if(PICODOOM_VIDEO_OUTPUT STREQUAL "hdmi")
    # PICODOOM_HDMI drives the #ifdef split in src/i_input_usbhid.cpp (USB
    # HID host moves from core1 to core0 -- core1 belongs to the DVI encode
    # loop exclusively, see src/i_video_dvi.cpp) and src/PicoDoom.cpp's
    # matching init-call choice.
    target_compile_definitions(PicoDoom PRIVATE PICODOOM_HDMI=1)
    # RP2350B's onboard TMDS pins (GPIO32-39) are >=32 -- the PIO block only
    # sees one 32-pin window at a time, and this (plus src/i_video_dvi.cpp's
    # own pio_set_gpio_base() call before dvi_init()) lets it move that
    # window for DVI's PIO block specifically. Explicit here because this
    # build's uSD wiring may not need it itself -- don't rely on the
    # LCD-build comment below, which is about a different PIO consumer.
    target_compile_definitions(PicoDoom PRIVATE PICO_PIO_USE_GPIO_BASE=1)
endif()
# PICO_PIO_USE_GPIO_BASE=1 no longer needs setting here for the LCD build:
# pico_toolset_sdcard links pico_fatfs PUBLIC, and
# third_party/pico-toolset/cmake/pico_fatfs.cmake already sets it as an
# INTERFACE definition on that target for every consumer (this board's uSD
# MISO pin, GPIO40, is >31 and pico_fatfs's generated PIO driver would
# otherwise silently alias it to GPIO8). The HDMI build sets it explicitly
# above as well, for DVI's own PIO block -- redundant but harmless on that
# build (both request the same value).

# Linking tinyusb_host (via pico_toolset_usb_hid, above) flips FOUR separate
# SDK defaults that only exist to detect "is this app using TinyUSB itself,
# beyond stdio_usb" -- every one is `!LIB_TINYUSB_HOST && !LIB_TINYUSB_DEVICE`-
# gated, and LIB_TINYUSB_HOST becomes true the moment tinyusb_host is linked,
# so leaving any of them at their new (off) default kills the native USB CDC
# console (this whole project's primary debugging channel) silently, or
# fails the build to link. Pico-Toolset's pico_toolset_usb_hid target sets
# these PUBLIC on itself (components/usb_hid/CMakeLists.txt) precisely so
# every consumer gets them for free instead of re-deriving and copy-pasting
# this exact block -- see that target for the full derivation of what each
# one does and why it was found the hard way.

pico_enable_stdio_usb(PicoDoom 1)
pico_enable_stdio_uart(PicoDoom 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoDoom)


