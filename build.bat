@echo off
rem Builds doom-mc-server.exe with MSYS2 UCRT64 gcc (gcc and g++ must be on PATH).
setlocal
if not exist obj mkdir obj
set DG=dg\doomgeneric
set SRCS=dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric
set CFLAGS=-O2 -w -fcommon -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I%DG%
set OBJS=
for %%f in (%SRCS%) do (
  if not exist obj\%%f.o gcc %CFLAGS% -c %DG%\%%f.c -o obj\%%f.o || exit /b 1
  call set OBJS=%%OBJS%% obj\%%f.o
)
gcc %CFLAGS% -c glue.c -o obj\glue.o || exit /b 1
gcc %CFLAGS% -I. -c soundglue.c -o obj\soundglue.o || exit /b 1
g++ -O2 -std=c++17 -Wall -Wno-unused-function -I%DG% main.cpp obj\glue.o obj\soundglue.o %OBJS% -o doom-mc-server.exe -lws2_32 -lwinmm -lz -lm || exit /b 1
echo built doom-mc-server.exe
