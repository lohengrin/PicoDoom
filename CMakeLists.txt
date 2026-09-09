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
set(CMAKE_CXX_STANDARD 17)

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

SET(DOOMSRC
doom/doomdef.c
doom/doomstat.c
doom/dstrings.c
doom/i_system.c
#doom/i_sound.c     # Linux OSS — not linkable on Pico
#doom/i_video.c     # X11 — not linkable on Pico
#doom/i_net.c       # Linux IPX — not linkable on Pico
#doom/i_video_pimoroni.c   # also X11 (misnomer)
doom/i_video_null.c
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
foreach(f ${DOOMSRC})
    if(NOT f MATCHES "d_main\\.c$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-std=gnu90")
    endif()
endforeach()

# rest of your project
add_executable(PicoDoom
src/PicoDoom.cpp
src/Psram.cpp
src/sd_stdio.c
${DOOMSRC}
)

# pico_fatfs (uSD/FatFs), hardware_psram (QMI PSRAM), hardware_pio (PIO-SPI
# base=16 for the uSD), on top of pico_stdlib.
target_link_libraries(PicoDoom
    pico_stdlib
    pico_fatfs
    hardware_psram
    hardware_pio
)

target_include_directories(PicoDoom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/doom
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(PicoDoom PRIVATE PICO)
target_compile_definitions(PicoDoom PRIVATE PICO_PIO_USE_GPIO_BASE=1)

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


