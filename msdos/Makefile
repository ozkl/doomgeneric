# Doom for MS-DOS / FreeDOS (DJGPP, 32-bit DPMI)
#
#   make            doom.exe with CWSDPMI embedded (runs on its own)
#   make msdos_original
#                   doom.exe that needs CWSDPMI.EXE next to it (or any
#                   other DPMI host: Windows 9x, HDPMI32, ...)
#   make run        run it in DOSBox with a WAD (WAD=path/to/doom.wad)
#   make clean
#
# The compiler: a local DJGPP cross compiler (i586-pc-msdosdjgpp-gcc on the
# PATH) when there is one, otherwise the djfdyuruiry/djgpp Docker image.
# Force either with DOCKER=1 / DOCKER=0.
#
# As the bare-metal build, Doom (../doomgeneric) is compiled unchanged, and
# so are the port glue (doomgeneric_baremetal.c, i_sound_baremetal.c), the
# sound mixer and the OPL3 General MIDI synth from ../baremetal.
# dos_platform.cpp replaces the bare-metal kernel and its sound drivers:
# VGA mode 13h, IRQ 1 keyboard, INT 33h mouse, PIT clock, Sound Blaster.
#
# Runs on a 386 or later with a 387 (or a 486DX); without one, put
# EMU387.DXE (DJGPP v2misc) next to it. 8 MB of RAM or more. Put the WAD in
# the same directory (or set DOOMWADDIR). Sound: BLASTER=A220 I5 D1.
#
# Net games: the original IPXSETUP.EXE / SERSETUP.EXE start it as they start
# DOOM.EXE (they run DOOM2.EXE if there is one, else DOOM.EXE), and it can
# play against the original DOOM.EXE: same IWAD, same version (1.9).
# When every machine runs this port, free look stays on; with an original
# DOOM.EXE in the game, everyone plays vanilla (mouse-forward, autoaim).
#   ipxsetup -nodes 2 -deathmatch -warp 1 3
VERSION = 20261027

DOOM    := ../doomgeneric
BM      := ../baremetal
BUILD   := build

DOS_TARGET      = doom.exe
DOS_COFF        = doom
COFF_TARGET     = doom.coff
EMBEDDED_TARGET = embedded.exe

DJGPP_IMAGE = djfdyuruiry/djgpp
CSDPMI_URL  = http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip

USER_ID  = $(shell id -u)
GROUP_ID = $(shell id -g)

CROSS ?= i586-pc-msdosdjgpp-
ifeq ($(origin DOCKER),undefined)
DOCKER := $(if $(shell command -v $(CROSS)gcc 2>/dev/null),0,1)
endif

ifeq ($(DOCKER),1)
# The whole repository is mounted, so ../doomgeneric and ../baremetal are reachable.
RUN      = docker run --rm -v $(abspath ..):/src:z -w /src/msdos -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c
CC_DOS   = gcc
CXX_DOS  = g++
EXE2COFF = exe2coff
else
RUN      = /bin/sh -c
CC_DOS   = $(CROSS)gcc
CXX_DOS  = $(CROSS)g++
EXE2COFF = $(shell command -v $(CROSS)exe2coff 2>/dev/null || echo $(dir $(shell command -v $(CROSS)gcc))../i586-pc-msdosdjgpp/bin/exe2coff)
endif

# A 386 with a 387, tuned for a Pentium. -DBAREMETAL: the sound-card line in
# Doom's Sound Volume menu (bm_audio.h).
ARCH     = -march=i386 -mtune=i586
DOOMDEFS = -DBAREMETAL -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND
CFLAGS   = -O2 $(ARCH) -std=gnu99 -w $(DOOMDEFS) -I$(BM)/overrides -I$(DOOM) -I$(BM)
CXXFLAGS = -O2 $(ARCH) -std=gnu++17 -fno-rtti -fno-exceptions -w -I$(DOOM) -I$(BM) -I.
LDFLAGS  = -s

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
CXX_SRCS := dos_platform.cpp $(BM)/mixer.cpp $(BM)/music.cpp $(BM)/opl/opl.cpp $(BM)/opl/dbopl.cpp

HDRS     := $(wildcard $(DOOM)/*.h) $(BM)/bm.h $(BM)/bm_audio.h $(BM)/audio.hpp $(BM)/mixer.hpp $(BM)/music.h $(wildcard $(BM)/opl/*.h)

all: msdos_embedded

pull-djgpp:
ifeq ($(DOCKER),1)
	docker pull $(DJGPP_IMAGE)
endif

get-csdpmi:
	@if [ ! -d "csdpmi" ]; then \
		wget $(CSDPMI_URL); \
		unzip -o csdpmi7b.zip -d csdpmi; \
	fi

# One compiler run per language, then link (no make inside the container).
$(COFF_TARGET): $(C_SRCS) $(CXX_SRCS) $(HDRS) Makefile | pull-djgpp
	$(RUN) "mkdir -p $(BUILD)/c $(BUILD)/cxx && \
		cd $(BUILD)/c && $(CC_DOS) $(CFLAGS:-I%=-I../../%) -c $(addprefix ../../,$(C_SRCS)) && cd ../.. && \
		cd $(BUILD)/cxx && $(CXX_DOS) $(CXXFLAGS:-I%=-I../../%) -c $(addprefix ../../,$(CXX_SRCS)) && cd ../.. && \
		$(CXX_DOS) $(LDFLAGS) $(BUILD)/c/*.o $(BUILD)/cxx/*.o -lm -o $(DOS_TARGET) && \
		$(EXE2COFF) $(DOS_TARGET) && mv $(DOS_COFF) $(COFF_TARGET)"

# Needs CWSDPMI.EXE (or another DPMI host) at run time.
msdos_original: $(COFF_TARGET) get-csdpmi
	$(RUN) "$(CXX_DOS) $(LDFLAGS) $(BUILD)/c/*.o $(BUILD)/cxx/*.o -lm -o $(DOS_TARGET)"
	cp csdpmi/bin/CWSDPMI.EXE .

# CWSDPMI's stub in front: one file that runs on plain DOS.
msdos_embedded: $(COFF_TARGET) get-csdpmi
	@if [ ! -f "csdpmi/bin/CWSDSTUB.EXE" ]; then \
		echo "ERROR: CWSDSTUB.EXE not found in csdpmi/bin directory!"; \
		echo "Make sure you have the complete CSDPMI package."; \
		exit 1; \
	fi
	cat csdpmi/bin/CWSDSTUB.EXE $(COFF_TARGET) > $(EMBEDDED_TARGET)
	mv $(EMBEDDED_TARGET) $(DOS_TARGET)
	chmod +x $(DOS_TARGET)
	@echo "Created $(DOS_TARGET) with embedded DPMI extender"

msdos: msdos_embedded

# DOSBox with a Sound Blaster 16 and a WAD copied in.
WADDIR ?= /usr/share/games/doom
WAD    ?= $(firstword $(wildcard *.wad) $(wildcard ../baremetal/*.wad) $(wildcard $(addprefix $(WADDIR)/,doom.wad doom2.wad doom1.wad freedoom2.wad freedoom1.wad)))
run: msdos_embedded
	@test -n "$(WAD)" || { echo "No WAD found: make run WAD=path/to/doom.wad"; exit 1; }
	@mkdir -p $(BUILD)/dosc
	cp $(DOS_TARGET) $(BUILD)/dosc/
	for w in $(WAD); do cp "$$w" "$(BUILD)/dosc/$$(basename "$$w" | tr a-z A-Z)"; done
	printf '[cpu]\ncore=dynamic\ncputype=auto\ncycles=max\n[dosbox]\nmachine=svga_s3\nmemsize=32\nvesa_modes=all\nvmemsize=8\n[sblaster]\nsbtype=sb16\nsbbase=220\nirq=7\ndma=1\nhdma=5\n[autoexec]\nset BLASTER=A220 I7 D1 H5 T6\n' > $(BUILD)/dosbox.conf
	dosbox -conf $(BUILD)/dosbox.conf -c "mount c $(BUILD)/dosc" -c "c:" -c "$(DOS_TARGET) $(ARGS)"

clean:
	rm -rf $(BUILD) $(DOS_TARGET) $(COFF_TARGET) $(EMBEDDED_TARGET) $(DOS_COFF) CWSDPMI.EXE || true
	rm -rf csdpmi csdpmi7b.zip csdpmi7b.zip.* || true

.PHONY: all pull-djgpp get-csdpmi msdos msdos_original msdos_embedded run clean
