cmake_minimum_required(VERSION 3.13)

#Defualt to pico_w
if(NOT DEFINED PICO_BOARD)
	SET(PICO_BOARD pico_w)
endif()

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(PicoDoom C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

SET(DOOMSRC
doom/doomdef.c
doom/doomstat.c
doom/dstrings.c
doom/i_system.c
#doom/i_sound.c
#doom/i_video.c
#doom/i_net.c
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

# rest of your project
add_executable(PicoDoom
src/PicoDoom.cpp
src/ssd1306.cpp
src/ssd1306.h
src/font.h
${DOOMSRC}
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(PicoDoom 
    pico_stdlib
    hardware_i2c
    hardware_adc
)

target_compile_definitions(PicoDoom PRIVATE PICO)

IF (${PICO_BOARD} STREQUAL "pico_w")
target_link_libraries(PicoDoom 
    pico_cyw43_arch_none
)
ENDIF()

pico_enable_stdio_usb(PicoDoom 1)
pico_enable_stdio_uart(PicoDoom 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoDoom)


