# vkernel userspace - Chocolate Quake
# Copyright (C) vkernel authors
# Copyright (C) 1996-1997 Id Software, Inc.
# Copyright (C) Henrique Barateli (Chocolate Quake)
#
# Builds chocolate-quake as a static PIE binary linked against newlib + vkernel CRT.
# Prerequisites: run scripts/setup_newlib.sh once, then make -C ../libc

CC      ?= gcc
AR      ?= ar

# -----------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------
QSRC           := ../quake-src/src
USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
KERNEL_INCLUDE := ../../include
SDL_SHIM       := sdl_shim

# -----------------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------------
CFLAGS  := -std=c11
CFLAGS  += -D_GNU_SOURCE
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
CFLAGS  += -mno-red-zone
CFLAGS  += -ffunction-sections -fdata-sections
ifdef DEBUG
CFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CFLAGS  += -O2 -DNDEBUG
endif
CFLAGS  += -Wall -Wno-unused-parameter -Wno-unused-variable
CFLAGS  += -Wno-missing-field-initializers -Wno-sign-compare
CFLAGS  += -Wno-implicit-fallthrough -Wno-unused-function
CFLAGS  += -Wno-pointer-sign -Wno-int-conversion
CFLAGS  += -isystem $(SYSROOT)/include
CFLAGS  += -include math.h

# SDL shim (must come before any system SDL headers)
CFLAGS  += -I$(SDL_SHIM)

# Module public include paths
CFLAGS  += -I$(QSRC)/camera/include
CFLAGS  += -I$(QSRC)/client/include
CFLAGS  += -I$(QSRC)/cmd/include
CFLAGS  += -I$(QSRC)/common/include
CFLAGS  += -I$(QSRC)/console/include
CFLAGS  += -I$(QSRC)/crc/include
CFLAGS  += -I$(QSRC)/end_screen/include
CFLAGS  += -I$(QSRC)/host/include
CFLAGS  += -I$(QSRC)/input/include
CFLAGS  += -I$(QSRC)/mathlib/include
CFLAGS  += -I$(QSRC)/memory/include
CFLAGS  += -I$(QSRC)/menu/include
CFLAGS  += -I$(QSRC)/model/include
CFLAGS  += -I$(QSRC)/net/include
CFLAGS  += -I$(QSRC)/progs/include
CFLAGS  += -I$(QSRC)/renderer/include
CFLAGS  += -I$(QSRC)/screen/include
CFLAGS  += -I$(QSRC)/server/include
CFLAGS  += -I$(QSRC)/sound/include
CFLAGS  += -I$(QSRC)/status_bar/include
CFLAGS  += -I$(QSRC)/sys/include
CFLAGS  += -I$(QSRC)/video/include
CFLAGS  += -I$(QSRC)/wad/include

# Module private src include paths (for files that include internal headers)
CFLAGS  += -I$(QSRC)/end_screen/src
CFLAGS  += -I$(QSRC)/net/src
CFLAGS  += -I$(QSRC)/video/src
CFLAGS  += -I$(QSRC)/input/src
CFLAGS  += -I$(QSRC)/sound/src

# VK / local includes
CFLAGS  += -I$(USER_INCLUDE)
CFLAGS  += -I$(KERNEL_INCLUDE)
CFLAGS  += -I.

# Linker flags
LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)

CRT0 := $(LIBC_DIR)/crt0.o
LIBS := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

# -----------------------------------------------------------------------
# Upstream source files (excluding replaced / skipped files)
# -----------------------------------------------------------------------

CAMERA_SRCS := \
	$(QSRC)/camera/src/chase.c \
	$(QSRC)/camera/src/view.c

CLIENT_SRCS := \
	$(QSRC)/client/src/cl_demo.c \
	$(QSRC)/client/src/cl_input.c \
	$(QSRC)/client/src/cl_main.c \
	$(QSRC)/client/src/cl_parse.c \
	$(QSRC)/client/src/cl_tent.c

CMD_SRCS := \
	$(QSRC)/cmd/src/cmd.c

COMMON_SRCS := \
	$(QSRC)/common/src/com_argv.c \
	$(QSRC)/common/src/com_byte.c \
	$(QSRC)/common/src/com_ext.c \
	$(QSRC)/common/src/com_fs.c \
	$(QSRC)/common/src/com_init.c \
	$(QSRC)/common/src/com_link.c \
	$(QSRC)/common/src/com_msg.c \
	$(QSRC)/common/src/com_sizebuf.c \
	$(QSRC)/common/src/com_stdio.c \
	$(QSRC)/common/src/com_stdlib.c \
	$(QSRC)/common/src/com_string.c \
	$(QSRC)/common/src/com_token.c \
	$(QSRC)/common/src/com_va.c

CONSOLE_SRCS := \
	$(QSRC)/console/src/console.c \
	$(QSRC)/console/src/cvar.c

CRC_SRCS := \
	$(QSRC)/crc/src/crc.c

ENDSCREEN_SRCS := \
	$(QSRC)/end_screen/src/es_buffer.c \
	$(QSRC)/end_screen/src/es_font.c \
	$(QSRC)/end_screen/src/es_main.c \
	$(QSRC)/end_screen/src/es_palette.c \
	$(QSRC)/end_screen/src/es_time.c

HOST_SRCS := \
	$(QSRC)/host/src/host.c \
	$(QSRC)/host/src/host_cmd.c

INPUT_SRCS := \
	$(QSRC)/input/src/keys.c

MATHLIB_SRCS := \
	$(QSRC)/mathlib/src/mathlib.c

MEMORY_SRCS := \
	$(QSRC)/memory/src/zone.c

MENU_SRCS := \
	$(QSRC)/menu/src/menu.c

MODEL_SRCS := \
	$(QSRC)/model/src/model.c

NET_SRCS := \
	$(QSRC)/net/src/net_dgrm.c \
	$(QSRC)/net/src/net_loop.c \
	$(QSRC)/net/src/net_main.c \
	$(QSRC)/net/src/net_poll.c \
	$(QSRC)/net/src/net_socket.c \
	$(QSRC)/net/src/net_vcr.c

PROGS_SRCS := \
	$(QSRC)/progs/src/pr_cmds.c \
	$(QSRC)/progs/src/pr_edict.c \
	$(QSRC)/progs/src/pr_exec.c

RENDERER_SRCS := \
	$(QSRC)/renderer/src/d_edge.c \
	$(QSRC)/renderer/src/d_fill.c \
	$(QSRC)/renderer/src/d_init.c \
	$(QSRC)/renderer/src/d_modech.c \
	$(QSRC)/renderer/src/d_part.c \
	$(QSRC)/renderer/src/d_polyse.c \
	$(QSRC)/renderer/src/draw.c \
	$(QSRC)/renderer/src/d_scan.c \
	$(QSRC)/renderer/src/d_sky.c \
	$(QSRC)/renderer/src/d_sprite.c \
	$(QSRC)/renderer/src/d_surf.c \
	$(QSRC)/renderer/src/d_vars.c \
	$(QSRC)/renderer/src/d_zpoint.c \
	$(QSRC)/renderer/src/nonintel.c \
	$(QSRC)/renderer/src/r_aclip.c \
	$(QSRC)/renderer/src/r_alias.c \
	$(QSRC)/renderer/src/r_bsp.c \
	$(QSRC)/renderer/src/r_draw.c \
	$(QSRC)/renderer/src/r_edge.c \
	$(QSRC)/renderer/src/r_efrag.c \
	$(QSRC)/renderer/src/r_light.c \
	$(QSRC)/renderer/src/r_main.c \
	$(QSRC)/renderer/src/r_misc.c \
	$(QSRC)/renderer/src/r_part.c \
	$(QSRC)/renderer/src/r_sky.c \
	$(QSRC)/renderer/src/r_sprite.c \
	$(QSRC)/renderer/src/r_surf.c \
	$(QSRC)/renderer/src/r_vars.c

SCREEN_SRCS := \
	$(QSRC)/screen/src/screen.c

SERVER_SRCS := \
	$(QSRC)/server/src/sv_main.c \
	$(QSRC)/server/src/sv_move.c \
	$(QSRC)/server/src/sv_phys.c \
	$(QSRC)/server/src/sv_user.c \
	$(QSRC)/server/src/sv_world.c

SOUND_SRCS := \
	$(QSRC)/sound/src/bgmusic.c \
	$(QSRC)/sound/src/snd_dma.c \
	$(QSRC)/sound/src/snd_mem.c \
	$(QSRC)/sound/src/snd_mix.c \
	$(QSRC)/sound/src/snd_mp3tag.c \
	$(QSRC)/sound/src/snd_wave.c

STATUSBAR_SRCS := \
	$(QSRC)/status_bar/src/sbar.c

VIDEO_SRCS := \
	$(QSRC)/video/src/vid_main.c \
	$(QSRC)/video/src/vid_menu.c \
	$(QSRC)/video/src/vid_modes.c

WAD_SRCS := \
	$(QSRC)/wad/src/wad.c

UPSTREAM_SRCS := \
	$(CAMERA_SRCS) \
	$(CLIENT_SRCS) \
	$(CMD_SRCS) \
	$(COMMON_SRCS) \
	$(CONSOLE_SRCS) \
	$(CRC_SRCS) \
	$(ENDSCREEN_SRCS) \
	$(HOST_SRCS) \
	$(INPUT_SRCS) \
	$(MATHLIB_SRCS) \
	$(MEMORY_SRCS) \
	$(MENU_SRCS) \
	$(MODEL_SRCS) \
	$(NET_SRCS) \
	$(PROGS_SRCS) \
	$(RENDERER_SRCS) \
	$(SCREEN_SRCS) \
	$(SERVER_SRCS) \
	$(SOUND_SRCS) \
	$(STATUSBAR_SRCS) \
	$(VIDEO_SRCS) \
	$(WAD_SRCS)

# -----------------------------------------------------------------------
# VK platform replacement files
# -----------------------------------------------------------------------

VK_SRCS := \
	main_vk.c \
	sys_vk.c \
	in_vk.c \
	vid_vk.c \
	snd_vk.c \
	snd_codec_vk.c \
	net_drivers_vk.c \
	net_udp_vk.c \
	es_window_vk.c

# -----------------------------------------------------------------------
# Object file generation
# -----------------------------------------------------------------------

OBJ_DIR := obj

# Upstream: strip leading path and module dir to get a flat obj name.
# $(QSRC)/module/src/file.c  →  obj/module__file.o
upstream_obj = $(OBJ_DIR)/up__$(subst /,__,$(patsubst $(QSRC)/%,%,$(1:.c=.o)))

UPSTREAM_OBJS := $(foreach s,$(UPSTREAM_SRCS),$(call upstream_obj,$(s)))
VK_OBJS       := $(patsubst %.c,$(OBJ_DIR)/%.o,$(VK_SRCS))

ALL_OBJS := $(UPSTREAM_OBJS) $(VK_OBJS)

TARGET := quake.vbin

.PHONY: all clean distclean

all: $(TARGET)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# Compile upstream sources
define compile_upstream
$(call upstream_obj,$(1)): $(1) | $(OBJ_DIR)
	@echo "  CC      $$<"
	@$(CC) $(CFLAGS) -c $$< -o $$@
endef
$(foreach s,$(UPSTREAM_SRCS),$(eval $(call compile_upstream,$(s))))

# Compile VK platform sources
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
