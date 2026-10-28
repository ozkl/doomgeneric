# Doom for Linux (SDL2), with IPX and serial net games built in.
#
#   make            ./doom, ./ipxsetup, ./sersetup, ./setup
#   make run        run it (WAD=path/to/doom.wad, ARGS="-warp 1 1")
#   make install    to $(PREFIX)/games (PREFIX=/usr/local)
#   make clean
#
# Needs a C/C++ compiler and SDL2 (Debian/Ubuntu: sudo apt install
# build-essential libsdl2-dev; Fedora: gcc-c++ SDL2-devel; Arch: sdl2).
#
# As the DOS and bare-metal builds, Doom (../doomgeneric) is compiled
# unchanged, and so are the port glue (doomgeneric_baremetal.c,
# i_sound_baremetal.c), the sound mixer and the OPL3 General MIDI synth from
# ../baremetal. linux_platform.cpp is the machine underneath (SDL2 video,
# keyboard, mouse, sound); net_*.cpp are IPXSETUP and SERSETUP, built into
# the game (./doom -ipx ...) and as programs of their own that start it, as
# on DOS (./ipxsetup -nodes 3 ...: setup_main.cpp).
VERSION = 20261028

DOOM    := ../doomgeneric
BM      := ../baremetal
BUILD   := build
TARGET  := doom

CC      ?= gcc
CXX     ?= g++
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)

# -DBAREMETAL: the sound-card line in Doom's Sound Volume menu (bm_audio.h)
# and DOS-style config (default.cfg, saves in the current directory).
# -DBM_HOSTED: the mixer runs on the main thread, not in an interrupt.
OPT      ?= -O2
DOOMDEFS = -DBAREMETAL -DBM_HOSTED -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND
CFLAGS   += $(OPT) -std=gnu99 -w -fno-strict-aliasing $(DOOMDEFS) -I$(BM)/overrides -I$(DOOM) -I$(BM)
CXXFLAGS += $(OPT) -std=gnu++17 -fno-exceptions -fno-rtti -Wall -Wno-unused-function -Wno-switch -DBM_HOSTED \
            -I$(DOOM) -I$(BM) -I. $(SDL_CFLAGS)
LDLIBS   += $(SDL_LIBS) -lm

DOOM_SRC := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
            f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound \
            i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc \
            m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats \
            p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw \
            r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables \
            v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric \
            mus2mid
C_SRCS   := $(addprefix $(DOOM)/,$(addsuffix .c,$(DOOM_SRC))) \
            $(BM)/doomgeneric_baremetal.c $(BM)/i_sound_baremetal.c $(BM)/opl/gmsynth.c $(BM)/opl/gmtables.c
NET_SRCS := net_common.cpp net_ipx.cpp net_serial.cpp
CXX_SRCS := linux_platform.cpp net_linux.cpp $(NET_SRCS) \
            $(BM)/mixer.cpp $(BM)/music.cpp $(BM)/opl/opl.cpp $(BM)/opl/dbopl.cpp

OBJS := $(patsubst %.c,$(BUILD)/c/%.o,$(notdir $(C_SRCS))) $(patsubst %.cpp,$(BUILD)/cxx/%.o,$(notdir $(CXX_SRCS)))
vpath %.c $(DOOM) $(BM) $(BM)/opl
vpath %.cpp . $(BM) $(BM)/opl

HDRS := $(wildcard $(DOOM)/*.h) $(wildcard $(BM)/*.h) $(wildcard $(BM)/*.hpp) $(wildcard $(BM)/opl/*.h) net_linux.h

NET_OBJS := $(patsubst %.cpp,$(BUILD)/cxx/%.o,$(NET_SRCS))

all: $(TARGET) ipxsetup sersetup setup

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# The DOS way: find the players, then start ./doom as their driver.
ipxsetup: setup_main.cpp setup_ui.cpp setup_ui.h $(NET_OBJS) net_linux.h Makefile
	$(CXX) $(CXXFLAGS) $(LDFLAGS) setup_main.cpp setup_ui.cpp $(NET_OBJS) -o $@
# SETUP.EXE: keys, mouse, video, sound (default.cfg, dgextra.cfg).
setup: setup.cpp keymap.h $(BM)/font.h Makefile
	$(CXX) $(CXXFLAGS) -I$(BM) $(LDFLAGS) setup.cpp $(SDL_LIBS) -o $@

sersetup: setup_main.cpp $(NET_OBJS) net_linux.h Makefile
	$(CXX) $(CXXFLAGS) -DSETUP_SERIAL $(LDFLAGS) setup_main.cpp $(NET_OBJS) -o $@

$(BUILD)/c/%.o: %.c $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cxx/%.o: %.cpp $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

WADDIR ?= /usr/share/games/doom
WAD    ?= $(firstword $(wildcard *.wad) $(wildcard $(addprefix $(WADDIR)/,doom2.wad doom.wad doom1.wad freedoom2.wad freedoom1.wad)))
run: $(TARGET)
	./$(TARGET) $(if $(WAD),-iwad $(WAD)) $(ARGS)

PREFIX ?= /usr/local
install: all
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/games/baremetaldoom
	install -D -m 755 ipxsetup $(DESTDIR)$(PREFIX)/games/baremetaldoom-ipxsetup
	install -D -m 755 sersetup $(DESTDIR)$(PREFIX)/games/baremetaldoom-sersetup
	install -D -m 755 setup $(DESTDIR)$(PREFIX)/games/baremetaldoom-setup

clean:
	rm -rf $(BUILD) $(TARGET) ipxsetup sersetup setup

.PHONY: all run install clean
