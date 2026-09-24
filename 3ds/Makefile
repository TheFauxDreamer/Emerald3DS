# Host half of the 3DS port. This Makefile links the ARM11 game archive with
# libctru.
#
# Build order, from the repo root:
#   make tools && make generated              # decomp tools, generated headers
#   python3 tools/generate_wasm_assets.py     # makes build/assets
#   rp2350/gen_sound_assets.sh                # once: wav to bin, mid to song .s
#   3ds/build_objs.sh                         # makes 3ds/build/libpokeemerald.a
#   make -C 3ds                               # makes 3ds/emerald3ds.{cia,3ds}
#
# This Makefile compiles only host-side sources. They must never see the game's
# headers (see 3ds/bridge.h), because u8/u16/u32 and string.h collide with the
# libctru ones.
#
# The toolchain paths are written here, not taken from $(DEVKITARM)/3ds_rules.
# That file expects the build-dir and VPATH layout of the stock template, which
# conflicts with the explicit rules below.

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

ifeq ($(wildcard $(DEVKITARM)/bin/arm-none-eabi-gcc),)
$(error devkitARM not found at $(DEVKITARM). Install devkitPro, or set DEVKITPRO)
endif

PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
CTRULIB  := $(DEVKITPRO)/libctru
PORTLIBS := $(DEVKITPRO)/portlibs/3ds
TOOLS    := $(DEVKITPRO)/tools/bin

TARGET   := emerald3ds
BUILD    := build
GAME_LIB := $(BUILD)/libpokeemerald.a

APP_TITLE       := Pokemon Emerald 3DS
APP_DESCRIPTION := Experience Pokemon Emerald with second-screen features!
APP_AUTHOR      := TheFauxDreamer

# Home Menu identity. All three assets are in meta/. See meta/README.md for
# their specifications. The tools apply them exactly and do not round.
#
# If meta/icon.png does not exist, the build uses the devkitPro default icon.
# When the file exists, the build uses it. No change to this file is necessary.
APP_ICON        := $(firstword $(wildcard meta/icon.png) $(CTRULIB)/default_icon.png)

# Banner inputs. The build makes a banner only when both exist, because
# bannertool does not make a banner without audio.
APP_BANNER_IMG  := meta/banner.png
APP_BANNER_WAV  := meta/banner.wav

# The file rp2350/ppu.c is host-side. It includes only <stdint.h> and its own
# header, and gets the GBA region pointers at run time from ppu_set_memory().
# Thus this port and the RP2350 port use the same byte-exact rasterizer.
HOST_SRCS := host/main.c host/video.c host/audio.c host/save.c host/settings.c \
             host/log.c host/io_thread.c host/achievements.c host/link.c
OBJS      := $(patsubst host/%.c,$(BUILD)/%.o,$(HOST_SRCS)) $(BUILD)/ppu.o

# This must be the same as ARCH in 3ds/build_objs.sh. If not, the game archive
# and libctru use different ABIs, and the link is wrong with no error.
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations

# Boot diagnostics:
# - Traces through svcOutputDebugString, which show in the log of an emulator.
# - A blue and green splash before AgbMain.
# - Moving bars in the pillarbox of the top screen, which show that the frame
#   loop runs.
#
# They are off by default. To turn them on, build with the command below, and
# give the same value to 3ds/build_objs.sh. See 3ds/host/trace.h.
#   make -C 3ds CTR_BOOT_DIAG=1
CTR_BOOT_DIAG ?= 0

# Run the rasterizer on a second CPU core (core 2 on a New 3DS, else core 1), at
# the same time as the bottom-screen paint. To build the single-core path for an
# A/B comparison, use `make -C 3ds CTR_PPU_THREAD=0`. The port also uses that
# path if it cannot create the thread. See 3ds/host/video.c. This flag is
# host-only, so 3ds/build_objs.sh needs no matching flag.
CTR_PPU_THREAD ?= 1

# Per-pass timers inside the rasterizer, for finding where a slow scene spends
# its time. rp2350/ppu.c has had them written all along behind PPU_PROFILE; this
# is the switch that compiles them in, and 3ds/host/video.c supplies the clock
# and prints one line every 600 frames.
#
# Off by default, and it must stay off in a build anyone plays. PSLOT fires per
# background per priority per line, about 1440 svcGetSystemTick calls a frame,
# which reads 5 to 15% high on an Old 3DS. The SPLIT is the measurement; the
# totals are not comparable with a normal build's `prof ppu`.
#
# CI is the only full build here and does not parametrise this, so to take a
# measurement: set this to 1, push, capture the log, then set it back.
#   make -C 3ds CTR_PPU_PROFILE=1
# Host-only, like CTR_PPU_THREAD: 3ds/build_objs.sh needs no matching flag.
CTR_PPU_PROFILE ?= 0

# Each make run sets these again, so the boot log always shows the correct
# build. The git description identifies the build. The timestamp shows the
# difference between two builds of the same tree. The description alone is
# CTR_BUILD_ID, which the title screen shows in the corner of the bottom screen
# (Ctr3dsBuildId in ../bridge.h).
#
# A build off main shows the git description alone. Every other branch shows its
# name first, as "branch/description", so a test build on a console cannot be
# mistaken for a main build. That matters most when two consoles must run the
# same build, as local wireless needs.
#
# The branch name does not come from git alone. CI checks out a detached HEAD,
# where git reports "HEAD", so GitHub supplies the name instead: GITHUB_HEAD_REF
# on a pull request, GITHUB_REF_NAME on a push.
#
# The value becomes a C string literal below, so every character outside
# [A-Za-z0-9._-] is dropped and a slash in the branch name becomes a dash. It is
# cut to 24 characters, which keeps the title screen's corner text short enough
# to stay on the screen in the small font.
BUILD_ID    := $(shell \
    d=$$(git describe --always --dirty 2>/dev/null || echo nogit); \
    b="$${GITHUB_HEAD_REF:-$${GITHUB_REF_NAME:-$$(git rev-parse --abbrev-ref HEAD 2>/dev/null)}}"; \
    b=$$(printf '%s' "$$b" | tr '/' '-' | tr -cd 'A-Za-z0-9._-' | cut -c1-24); \
    if [ -z "$$b" ] || [ "$$b" = HEAD ] || [ "$$b" = main ]; then printf '%s' "$$d"; \
    else printf '%s/%s' "$$b" "$$d"; fi)
BUILD_STAMP := $(shell date -u +%Y-%m-%d\ %H:%M:%SZ) $(BUILD_ID)

# The build needs -MMD -MP. Without -MMD, each object depends only on its own .c
# file. Then a change to ../bridge.h, the interface between the two halves of
# the port, rebuilds only main.o (see FORCE below). The link uses the old
# settings.o, video.o, audio.o, save.o, log.o and ppu.o. A changed macro or enum
# value then has two values in one binary, with no error.
#
# The -MP option adds phony targets for the headers. Thus a deleted or renamed
# header does not stop the build with "no rule to make target".
CFLAGS   := $(ARCH) -g -Wall -O2 -fno-strict-aliasing -fomit-frame-pointer \
            -ffunction-sections -fdata-sections -MMD -MP \
            -DCTR_BOOT_DIAG=$(CTR_BOOT_DIAG) \
            -DCTR_PPU_THREAD=$(CTR_PPU_THREAD) \
            -DCTR_PPU_PROFILE=$(CTR_PPU_PROFILE) \
            -DCTR_BUILD_STAMP='"$(BUILD_STAMP)"' \
            -DCTR_BUILD_ID='"$(BUILD_ID)"' \
            -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include -I.
LDFLAGS  := $(ARCH) -specs=3dsx.specs -g -Wl,-Map,$(BUILD)/$(TARGET).map

# rp2350/ppu.c guards its timers on PPU_PROFILE, which the rp2350 build uses
# too, so the 3DS knob maps onto that name rather than renaming the guard.
ifeq ($(CTR_PPU_PROFILE),1)
CFLAGS   += -DPPU_PROFILE
endif

# Point the tail jump of the original mixer at itself.
#
# SoundMain (src/m4a_1.s) ends with `ldr r3, lt_SoundMainRAM_Buffer / bx r3`.
# That literal is `.word SoundMainRAM_Buffer + 1`. On a GBA, the buffer is an
# IWRAM copy that m4aSoundInit makes, because IWRAM has zero wait states.
#
# That is not possible here. IWRAM is an offset into gGbaMem (3ds/gba_mem.c), a
# usual .bss array. A CXI has no read/write/execute segment for code. Also, a
# copy gives no speed, because the ARM11 runs .text at full speed. Thus the
# buffer symbol is the routine, and src/m4a.c does not make the copy.
#
# The file 3ds/asm/m4a_arm11.s, in the game archive, supplies the symbol.
# Neither `.set` nor `ld --defsym` can do this. The assembler cannot resolve
# .set to a symbol in a different object. The linker evaluates --defsym before
# symbol resolution, so it gives 0 and the tail jump faults at address 0. See
# the comment in that file.
LIBS     := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib

MAKEROM  ?= makerom
BANNERTOOL ?= bannertool
RSF      := emerald3ds.rsf

.PHONY: all clean cia cci 3dsx

# CIA (to install on a console) and CCI/.3ds (to boot directly in an emulator).
#
# Not 3dsx, on purpose. A .3dsx loads at a variable address, so each absolute
# pointer needs a relocation at load time. The 3DSX relocation table uses whole
# 4-byte words. The script bytecode of Emerald puts a 4-byte pointer directly
# after a 1-byte opcode (.macro goto, asm/macros/event.inc). Thus thousands of
# these relocations are unaligned, and 3dsxtool stops with "Unaligned
# relocation!". A CXI loads at a fixed address with no relocation, so the same
# pointers are correct.
#
# See the header comment in emerald3ds.rsf.
all: $(TARGET).cia $(TARGET).3ds

cia: $(TARGET).cia
cci: $(TARGET).3ds

# The SMDH holds the Home Menu title and icon. It is optional. If the devkitPro
# default icon is missing, the build continues without an icon.
ifneq ($(wildcard $(APP_ICON)),)
SMDH_DEP  := $(TARGET).smdh
ICON_FLAG := -icon $(TARGET).smdh

$(TARGET).smdh: $(APP_ICON) | $(BUILD)
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@
else
SMDH_DEP  :=
ICON_FLAG :=
endif

# The banner is the top-screen image and jingle that show while the title is
# selected in the Home Menu. It is optional, as the icon is: with no art there
# is no banner, but the build continues.
#
# The build needs both inputs. The bannertool program fails when it gets an
# image and no audio. Thus a check on only one input would make a missing wav
# stop the build.
ifneq ($(and $(wildcard $(APP_BANNER_IMG)),$(wildcard $(APP_BANNER_WAV))),)
BANNER_DEP  := $(TARGET).bnr
BANNER_FLAG := -banner $(TARGET).bnr

# Two steps, not one, only to make the tune loop.
#
# The command `makebanner -a file.wav` converts the audio itself, with looping
# always off. The source of the tool calls convert_to_cwav(audio, false, 0, 0),
# and no flag changes it. Thus the jingle plays once and stops. A retail banner
# loops while the title stays selected.
#
# The command `makecwav` does the same conversion, with a loop flag. The command
# `makebanner -ca` accepts the finished CWAV in place of a wav. Thus this
# converts with -l true, then gives the result to makebanner. The output size is
# the same, with the loop flag set.
#
# The loop points are the default: the whole file, from frame 0 to the last
# frame. A shorter loop needs the frame numbers, which come from the music, not
# from the build.
$(BUILD)/$(TARGET).cwav: $(APP_BANNER_WAV) | $(BUILD)
	$(BANNERTOOL) makecwav -i $(APP_BANNER_WAV) -o $@ -l true

$(TARGET).bnr: $(APP_BANNER_IMG) $(BUILD)/$(TARGET).cwav | $(BUILD)
	$(BANNERTOOL) makebanner -i $(APP_BANNER_IMG) -ca $(BUILD)/$(TARGET).cwav -o $@
else
BANNER_DEP  :=
BANNER_FLAG :=
endif

$(TARGET).cia: $(TARGET).elf $(SMDH_DEP) $(BANNER_DEP) $(RSF)
	$(MAKEROM) -f cia -target t -exefslogo -o $@ -elf $(TARGET).elf -rsf $(RSF) $(ICON_FLAG) $(BANNER_FLAG)
	@echo "built ... $@"

$(TARGET).3ds: $(TARGET).elf $(SMDH_DEP) $(BANNER_DEP) $(RSF)
	$(MAKEROM) -f cci -target t -exefslogo -o $@ -elf $(TARGET).elf -rsf $(RSF) $(ICON_FLAG) $(BANNER_FLAG)
	@echo "built ... $@"

# This target is here only to reproduce the failure. See the comment on `all`.
3dsx: $(TARGET).elf $(SMDH_DEP)
	$(TOOLS)/3dsxtool $(TARGET).elf $(TARGET).3dsx $(if $(SMDH_DEP),--smdh=$(TARGET).smdh)

$(TARGET).elf: $(OBJS) $(GAME_LIB)
	@echo "linking ... $@"
	$(CC) $(LDFLAGS) $(OBJS) $(GAME_LIB) $(LIBDIRS) $(LIBS) -o $@

$(GAME_LIB):
	@echo "error: $@ missing. Run 3ds/build_objs.sh first." >&2; exit 1

# The file main.o holds the build stamp, which changes on each run. Thus make
# must build it each time. One small rebuild costs less than a log line that
# shows the wrong binary.
#
# This uses a prerequisite that is always out of date, not .PHONY. If a real
# file is .PHONY, make does not think it is a file, and never builds it. The
# link then fails with "cannot find build/main.o". The recipe still comes from
# the pattern rule below. This adds only a prerequisite.
.PHONY: FORCE
FORCE:

$(BUILD)/main.o: FORCE

$(BUILD)/%.o: host/%.c | $(BUILD)
	@echo "compiling ... $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ppu.o: ../rp2350/ppu.c | $(BUILD)
	@echo "compiling ... $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

# The dependency files that -MMD wrote on the last build. They do not exist on
# the first build, so this is -include, not include.
-include $(OBJS:.o=.d)

clean:
	@rm -rf $(BUILD)/*.o $(BUILD)/*.d $(TARGET).elf $(TARGET).3dsx $(TARGET).3ds \
	        $(TARGET).cia $(TARGET).smdh $(TARGET).bnr $(BUILD)/$(TARGET).cwav \
	        $(BUILD)/$(TARGET).map
