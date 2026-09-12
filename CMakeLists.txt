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

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(PicoDoom C CXX ASM)
set(CMAKE_C_STANDARD 11)
# 20, not 17: src/Ili9486Display.hpp (ported verbatim from TOM6809's
# validated driver for this exact panel) uses std::span, C++20-only.
set(CMAKE_CXX_STANDARD 20)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Pico-Toolset (submodule, third_party/pico-toolset) provides pico_fatfs
# (uSD/FatFs, with PICO_PIO_USE_GPIO_BASE=1 already set -- this board's uSD
# MISO pin, GPIO40, is >31 and pico_fatfs's generated PIO driver would
# otherwise silently alias it to GPIO8) and Pico-PIO-USB/tinyusb_host (this
# board's PIO-emulated USB *host* controller for keyboard/mouse input,
# PICO_DEFAULT_PIO_USB_DP_PIN/GPIO28-29 -- RP2350's own USB peripheral is
# native-device-only and already committed to stdio_usb's serial console).
# Phase 1: PSRAM, SD card (+ its opt-in POSIX/stdio syscall shim) and the
# fault handler are consumed via their pico_toolset_* targets below. ILI9486/
# USB_HID stay ON here because src/Ili9486Display.* and
# src/PicoUsbKeyboard.*/PicoUsbMouse.* still use their own local copies
# pending the Phase 2 follow-up, and this project still needs the
# pico_fatfs/pico_pio_usb/tinyusb_host targets they'd otherwise have set up
# via the FetchContent blocks this replaces.
set(PICO_TOOLSET_BUILD_SSD1306 OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_XPT2046 OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_I2S_AUDIO OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_RESET_BUTTONS OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_DVI_HDMI OFF CACHE BOOL "" FORCE)
set(PICO_TOOLSET_BUILD_SCREEN OFF CACHE BOOL "" FORCE)
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
doom/i_sound_null.c
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

# rest of your project
add_executable(PicoDoom
src/PicoDoom.cpp
src/sd_stdio.cpp
src/Ili9486Display.cpp
src/i_video_ili9486.cpp
src/PicoUsbKeyboard.cpp
src/PicoUsbMouse.cpp
src/i_input_usbhid.cpp
${DOOMSRC}
)

# pico_toolset_sdcard (uSD/FatFs + POSIX stdio shim), pico_toolset_psram
# (QMI PSRAM), pico_toolset_fault_handler (hard-fault -> next-boot report),
# hardware_spi + hardware_dma (ILI9486 LCD over SPI1, DMA-fed pixel pushes,
# still local -- see src/Ili9486Display.*), pico_pio_usb + tinyusb_host +
# pico_multicore (Phase 3 USB-PIO keyboard host on core1, still local -- see
# src/PicoUsbKeyboard.cpp; hcd_pio_usb.c comes in as an INTERFACE source of
# pico_pio_usb, set up by third_party/pico-toolset/cmake/pico_pio_usb.cmake),
# hardware_watchdog (doom/i_system.c's I_Quit -> watchdog_reboot()), on top
# of pico_stdlib.
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
    hardware_spi
    hardware_dma
    pico_pio_usb
    tinyusb_host
    pico_multicore
    hardware_watchdog
)

target_include_directories(PicoDoom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/doom
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(PicoDoom PRIVATE PICO)
# PICO_PIO_USE_GPIO_BASE=1 no longer needs setting here: pico_toolset_sdcard
# links pico_fatfs PUBLIC, and third_party/pico-toolset/cmake/pico_fatfs.cmake
# already sets it as an INTERFACE definition on that target for every
# consumer (this board's uSD MISO pin, GPIO40, is >31 and pico_fatfs's
# generated PIO driver would otherwise silently alias it to GPIO8).

# Linking tinyusb_host (above, for the USB-PIO keyboard) flips FOUR separate
# SDK defaults that only exist to detect "is this app using TinyUSB itself,
# beyond stdio_usb". Every one of these is `!LIB_TINYUSB_HOST &&
# !LIB_TINYUSB_DEVICE`-gated, and LIB_TINYUSB_HOST becomes true the moment
# tinyusb_host is linked -- with any of these left at their new (off)
# default, the native USB CDC console (this whole project's primary
# debugging channel) goes silently dead, or the build fails to link.
# Verified the hard way on TOM6809 (same board, same pico_pio_usb +
# tinyusb_host combination) -- ported its exact fix rather than
# rediscovering each one:
#  - USE_DEFAULT_DESCRIPTORS: stdio_usb_descriptors.c stops being compiled
#    in by default once the app "provides its own" -- this app doesn't,
#    the link fails (tud_descriptor_device_cb and friends undefined)
#    without forcing it back on.
#  - ENABLE_TINYUSB_INIT: controls whether stdio_usb_init() calls
#    tusb_init() at all. Off, and nothing else in this codebase calls it
#    either (PicoUsbKeyboard's tuh_init() only brings up the host role,
#    rhport 1) -- the native USB peripheral hardware is simply never
#    brought up. Not a timing/threading bug, the device is never
#    initialized, full stop.
#  - ENABLE_IRQ_BACKGROUND_TASK: guards stdio_usb_init()'s low-priority-IRQ
#    installation that actually pumps tud_task() in the background. Off,
#    and the device stack initializes (thanks to ENABLE_TINYUSB_INIT above)
#    but never runs: stdio_usb.c's own inline tud_task() calls are gated
#    behind stdio_usb_connected(), which can't become true until
#    enumeration completes -- which needs tud_task() to run. Chicken-and-
#    egg deadlock: USB HID keyboard works, but no /dev/ttyACM* ever
#    appears.
#  - The same gate appears a fourth time in pico_usb_reset's
#    usb_reset_config.h, controlling the vendor "Reset" interface
#    picotool's `load -x -f`/BOOTSEL-reboot depends on. Off, and the board
#    still enumerates as CDC but picotool reports "Unable to locate reset
#    interface on the device" -- BOOTSEL has to be entered by hand
#    (button/power-cycle) from then on. MS_OS_20_DESCRIPTOR_ITF=2 is not
#    optional alongside VIA_VENDOR_INTERFACE: stdio_usb_descriptors.c has
#    an unconditional static_assert tying the two together (2 matches
#    USBD_ITF_RPI_RESET, since CDC itself takes interfaces 0+1).
target_compile_definitions(PicoDoom PRIVATE
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=1
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF=2
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=1
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
)

pico_enable_stdio_usb(PicoDoom 1)
pico_enable_stdio_uart(PicoDoom 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoDoom)


