# vkernel userspace - Chocolate Doom
# Copyright (C) 2026 vkernel authors
# Copyright (C) 1993-1996 Id Software, Inc.
# Copyright (C) 2005-2014 Simon Howard (Chocolate Doom)
#
# Builds chocolate-doom as a static PIE binary linked against newlib + vkernel CRT.
# Prerequisites: run scripts/setup_newlib.sh once, then make -C ../libc

CC      ?= gcc
AR      ?= ar
OBJCOPY ?= objcopy

# Paths
DOOM_SRC       := ../doom-src/src
DOOM_GAME_SRC  := ../doom-src/src/doom
USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
KERNEL_INCLUDE := ../../include
SDL_SHIM       := sdl_shim

# Compiler flags
CFLAGS  := -std=c11
CFLAGS  += -D_GNU_SOURCE
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
CFLAGS  += -ffunction-sections -fdata-sections
ifdef DEBUG
CFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CFLAGS  += -O2 -DNDEBUG
endif
CFLAGS  += -Wall -Wno-unused-parameter -Wno-unused-variable
CFLAGS  += -Wno-missing-field-initializers -Wno-sign-compare
CFLAGS  += -Wno-implicit-fallthrough -Wno-unused-function
CFLAGS  += -isystem $(SYSROOT)/include
CFLAGS  += -I$(USER_INCLUDE)
CFLAGS  += -I$(KERNEL_INCLUDE)
CFLAGS  += -I$(SDL_SHIM)
CFLAGS  += -I.
CFLAGS  += -I$(DOOM_SRC)
CFLAGS  += -I$(DOOM_SRC)/doom
CFLAGS  += -DDISABLE_SDL2MIXER
CFLAGS  += -DDISABLE_SDL2NET

# Linker flags
LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)

CRT0   := $(LIBC_DIR)/crt0.o
LIBS   := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

# ---------- Source file lists ----------

# Common/game source files from chocolate-doom (excluding replaced i_* files)
COMMON_SRCS := \
	$(DOOM_SRC)/aes_prng.c \
	$(DOOM_SRC)/d_event.c \
	$(DOOM_SRC)/d_iwad.c \
	$(DOOM_SRC)/d_loop.c \
	$(DOOM_SRC)/d_mode.c \
	$(DOOM_SRC)/deh_io.c \
	$(DOOM_SRC)/deh_main.c \
	$(DOOM_SRC)/deh_mapping.c \
	$(DOOM_SRC)/deh_str.c \
	$(DOOM_SRC)/deh_text.c \
	$(DOOM_SRC)/m_argv.c \
	$(DOOM_SRC)/m_bbox.c \
	$(DOOM_SRC)/m_cheat.c \
	$(DOOM_SRC)/m_config.c \
	$(DOOM_SRC)/m_controls.c \
	$(DOOM_SRC)/m_fixed.c \
	$(DOOM_SRC)/m_misc.c \
	$(DOOM_SRC)/memio.c \
	$(DOOM_SRC)/midifile.c \
	$(DOOM_SRC)/midifallback.c \
	$(DOOM_SRC)/mus2mid.c \
	$(DOOM_SRC)/net_client.c \
	$(DOOM_SRC)/net_common.c \
	$(DOOM_SRC)/net_dedicated.c \
	$(DOOM_SRC)/net_io.c \
	$(DOOM_SRC)/net_loop.c \
	$(DOOM_SRC)/net_packet.c \
	$(DOOM_SRC)/net_petname.c \
	$(DOOM_SRC)/net_server.c \
	$(DOOM_SRC)/net_structrw.c \
	$(DOOM_SRC)/p_rejectpad.c \
	$(DOOM_SRC)/sha1.c \
	$(DOOM_SRC)/tables.c \
	$(DOOM_SRC)/v_diskicon.c \
	$(DOOM_SRC)/v_video.c \
	$(DOOM_SRC)/w_checksum.c \
	$(DOOM_SRC)/w_file.c \
	$(DOOM_SRC)/w_file_stdc.c \
	$(DOOM_SRC)/w_main.c \
	$(DOOM_SRC)/w_merge.c \
	$(DOOM_SRC)/w_wad.c \
	$(DOOM_SRC)/z_zone.c

# Doom game-specific source files
GAME_SRCS := \
	$(DOOM_GAME_SRC)/am_map.c \
	$(DOOM_GAME_SRC)/deh_ammo.c \
	$(DOOM_GAME_SRC)/deh_bexstr.c \
	$(DOOM_GAME_SRC)/deh_cheat.c \
	$(DOOM_GAME_SRC)/deh_doom.c \
	$(DOOM_GAME_SRC)/deh_frame.c \
	$(DOOM_GAME_SRC)/deh_misc.c \
	$(DOOM_GAME_SRC)/deh_ptr.c \
	$(DOOM_GAME_SRC)/deh_sound.c \
	$(DOOM_GAME_SRC)/deh_thing.c \
	$(DOOM_GAME_SRC)/deh_weapon.c \
	$(DOOM_GAME_SRC)/d_items.c \
	$(DOOM_GAME_SRC)/d_main.c \
	$(DOOM_GAME_SRC)/d_net.c \
	$(DOOM_GAME_SRC)/doomdef.c \
	$(DOOM_GAME_SRC)/doomstat.c \
	$(DOOM_GAME_SRC)/dstrings.c \
	$(DOOM_GAME_SRC)/f_finale.c \
	$(DOOM_GAME_SRC)/f_wipe.c \
	$(DOOM_GAME_SRC)/g_game.c \
	$(DOOM_GAME_SRC)/hu_lib.c \
	$(DOOM_GAME_SRC)/hu_stuff.c \
	$(DOOM_GAME_SRC)/info.c \
	$(DOOM_GAME_SRC)/m_menu.c \
	$(DOOM_GAME_SRC)/m_random.c \
	$(DOOM_GAME_SRC)/p_ceilng.c \
	$(DOOM_GAME_SRC)/p_doors.c \
	$(DOOM_GAME_SRC)/p_enemy.c \
	$(DOOM_GAME_SRC)/p_floor.c \
	$(DOOM_GAME_SRC)/p_inter.c \
	$(DOOM_GAME_SRC)/p_lights.c \
	$(DOOM_GAME_SRC)/p_map.c \
	$(DOOM_GAME_SRC)/p_maputl.c \
	$(DOOM_GAME_SRC)/p_mobj.c \
	$(DOOM_GAME_SRC)/p_plats.c \
	$(DOOM_GAME_SRC)/p_pspr.c \
	$(DOOM_GAME_SRC)/p_saveg.c \
	$(DOOM_GAME_SRC)/p_setup.c \
	$(DOOM_GAME_SRC)/p_sight.c \
	$(DOOM_GAME_SRC)/p_spec.c \
	$(DOOM_GAME_SRC)/p_switch.c \
	$(DOOM_GAME_SRC)/p_telept.c \
	$(DOOM_GAME_SRC)/p_tick.c \
	$(DOOM_GAME_SRC)/p_user.c \
	$(DOOM_GAME_SRC)/r_bsp.c \
	$(DOOM_GAME_SRC)/r_data.c \
	$(DOOM_GAME_SRC)/r_draw.c \
	$(DOOM_GAME_SRC)/r_main.c \
	$(DOOM_GAME_SRC)/r_plane.c \
	$(DOOM_GAME_SRC)/r_segs.c \
	$(DOOM_GAME_SRC)/r_sky.c \
	$(DOOM_GAME_SRC)/r_things.c \
	$(DOOM_GAME_SRC)/s_sound.c \
	$(DOOM_GAME_SRC)/sounds.c \
	$(DOOM_GAME_SRC)/statdump.c \
	$(DOOM_GAME_SRC)/st_lib.c \
	$(DOOM_GAME_SRC)/st_stuff.c \
	$(DOOM_GAME_SRC)/wi_stuff.c

# Our vkernel platform layer (replaces SDL-based files)
VK_SRCS := \
	i_main_vk.c \
	i_video_vk.c \
	i_sound_vk.c \
	i_timer_vk.c \
	i_system_vk.c \
	i_stubs_vk.c

ALL_SRCS := $(COMMON_SRCS) $(GAME_SRCS) $(VK_SRCS)

# Object files — flatten into obj/ directory
OBJ_DIR := obj
COMMON_OBJS := $(patsubst $(DOOM_SRC)/%.c,$(OBJ_DIR)/common_%.o,$(COMMON_SRCS))
GAME_OBJS   := $(patsubst $(DOOM_GAME_SRC)/%.c,$(OBJ_DIR)/game_%.o,$(GAME_SRCS))
VK_OBJS     := $(patsubst %.c,$(OBJ_DIR)/%.o,$(VK_SRCS))
ALL_OBJS    := $(COMMON_OBJS) $(GAME_OBJS) $(VK_OBJS)

TARGET := doom.vbin

.PHONY: all clean distclean

all: $(TARGET)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# Compile common sources
$(OBJ_DIR)/common_%.o: $(DOOM_SRC)/%.c | $(OBJ_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile game sources
$(OBJ_DIR)/game_%.o: $(DOOM_GAME_SRC)/%.c | $(OBJ_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile vkernel platform sources
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Link
$(TARGET): $(ALL_OBJS) $(CRT0)
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $(CRT0) $(ALL_OBJS) $(LIBS)
ifndef DEBUG
	@strip --strip-unneeded $@
endif
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -rf $(OBJ_DIR) $(TARGET)

distclean: clean
