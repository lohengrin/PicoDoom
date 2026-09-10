cmake_minimum_required(VERSION 3.13)

# Target board: Waveshare RP2350-PiZero. Use our boards/waveshare_rp2350_pizero.h
# (copied from the validated TOM6809 project) ahead of the SDK's, since it adds
# PICO_PSRAM_CS_PIN and PICO_DEFAULT_PIO_USB_DP_PIN the SDK's version lacks.
# PICO_BOARD must be defined BEFORE pico_sdk_import.cmake: its
# pico_pre_load_platform runs at include time and decides the toolchain/platform
# (rp2350 vs rp2040) from it.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
endif()
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards CACHE STRING "" FORCE)

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

# pico_fatfs -- FatFs (R0.15) with a Pico SPI/PIO backend for the uSD card.
# Same repo/config as TOM6809's pizero_lcd profile. PICO_PIO_USE_GPIO_BASE is
# required on this board: its uSD MISO pin (GPIO40) is >31 and pico_fatfs's
# generated PIO driver would otherwise silently alias it to GPIO8 (see
# src/sd_stdio.c). Set as INTERFACE so it reaches hardware_pio too.
include(FetchContent)
FetchContent_Declare(
    pico_fatfs
    GIT_REPOSITORY https://github.com/elehobica/pico_fatfs.git
    GIT_TAG main
)
FetchContent_MakeAvailable(pico_fatfs)
target_compile_definitions(pico_fatfs INTERFACE PICO_PIO_USE_GPIO_BASE=1)

# Pico-PIO-USB -- Phase 3 keyboard input (PICO_DEFAULT_PIO_USB_DP_PIN,
# GPIO28/29, see boards/waveshare_rp2350_pizero.h): RP2350's own USB
# peripheral is native-device-only and already committed to stdio_usb's
# serial console, so this is a second, PIO-emulated USB *host* controller,
# driven by TinyUSB configured as tinyusb_host. Same dependency/config as
# TOM6809's validated WAVESHARE_USB_HID_SUPPORT profile (src/ui_pico's
# PicoUsbHidInput) -- ported verbatim below rather than re-derived, since
# every one of these defines was found the hard way there (see each one's
# comment) and getting even one wrong either wedges the USB host silently
# or kills the serial console this whole project has been debugged through.
#
# Deliberately NOT pico-sdk's auto-detected tinyusb_pico_pio_usb target
# (family.cmake's check_and_add_pico_pio_usb_support() runs once during
# pico_sdk_init() above, before a PICO_PIO_USB_PATH set here could take
# effect) -- Pico-PIO-USB's own upstream CMakeLists.txt gives a
# self-contained `pico_pio_usb` INTERFACE library that doesn't need that
# detection at all; the one piece still needed from TinyUSB itself is the
# host-controller-driver glue (hcd_pio_usb.c), added as a source below from
# the vendored tree (PICO_TINYUSB_PATH is set by pico-sdk's own
# src/rp2_common/tinyusb/CMakeLists.txt during pico_sdk_init()).
FetchContent_Declare(
    pico_pio_usb
    GIT_REPOSITORY https://github.com/sekigon-gonnoc/Pico-PIO-USB.git
    GIT_TAG main
)
FetchContent_MakeAvailable(pico_pio_usb)
target_compile_definitions(pico_pio_usb INTERFACE PIO_USB_USE_TINYUSB)

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
foreach(f ${DOOMSRC})
    if(NOT f MATCHES "d_main\\.c$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-std=gnu90;-w")
    else()
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endforeach()

# rest of your project
add_executable(PicoDoom
src/PicoDoom.cpp
src/Psram.cpp
src/sd_stdio.c
src/Ili9486Display.cpp
src/i_video_ili9486.cpp
src/PicoUsbKeyboard.cpp
src/PicoUsbMouse.cpp
src/i_input_usbhid.cpp
${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
${DOOMSRC}
)

# pico_fatfs (uSD/FatFs), hardware_psram (QMI PSRAM), hardware_pio (PIO-SPI
# base=16 for the uSD), hardware_spi + hardware_dma (ILI9486 LCD over SPI1,
# DMA-fed pixel pushes), pico_pio_usb + tinyusb_host + pico_multicore
# (Phase 3 USB-PIO keyboard host on core1, see src/PicoUsbKeyboard.cpp), on
# top of pico_stdlib.
target_link_libraries(PicoDoom
    pico_stdlib
    pico_fatfs
    hardware_psram
    hardware_pio
    hardware_spi
    hardware_dma
    pico_pio_usb
    tinyusb_host
    pico_multicore
)

target_include_directories(PicoDoom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/doom
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(PicoDoom PRIVATE PICO)
target_compile_definitions(PicoDoom PRIVATE PICO_PIO_USE_GPIO_BASE=1)

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

# Hardware bring-up diagnostic: bisect which init stage kills USB enumeration
# without needing a debug probe. 0=stdio_init_all()+heartbeat only; 1=+PSRAM;
# 2=+uSD mount; 3=full normal boot (default, unchanged behavior); 4=+D_DoomMain
# up to FindResponseFile(); 5=+IdentifyVersion(). See src/PicoDoom.cpp and
# doom/d_main.c.
set(PICODOOM_DIAG_STAGE 3 CACHE STRING "Boot diagnostic stage: 0=stdio only, 1=+PSRAM, 2=+uSD mount, 3=full boot, 4=+FindResponseFile, 5=+IdentifyVersion")
target_compile_definitions(PicoDoom PRIVATE PICODOOM_DIAG_STAGE=${PICODOOM_DIAG_STAGE})

pico_enable_stdio_usb(PicoDoom 1)
pico_enable_stdio_uart(PicoDoom 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoDoom)


