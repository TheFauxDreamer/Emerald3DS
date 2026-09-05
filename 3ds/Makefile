# Host half of the 3DS port: links the ARM11 game archive against libctru.
#
# Build order from the repo root:
#   make tools && make generated              # decomp tools + generated headers
#   python3 tools/generate_wasm_assets.py     # -> build/assets
#   rp2350/gen_sound_assets.sh                # one-time: wav->bin, mid->song .s
#   3ds/build_objs.sh                         # -> 3ds/build/libpokeemerald.a
#   make -C 3ds                               # -> 3ds/emerald3ds.{cia,3ds}
#
# Only host-side sources are compiled here. They must never see the game's
# headers (see 3ds/bridge.h): u8/u16/u32 and string.h collide with libctru's.
#
# Toolchain paths are spelled out rather than pulled from $(DEVKITARM)/3ds_rules,
# which expects the stock template's build-dir/VPATH layout and would fight the
# explicit rules below.

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
APP_DESCRIPTION := Dual-screen native port
APP_AUTHOR      := pokeemerald-3ds
APP_ICON        := $(CTRULIB)/default_icon.png

# rp2350/ppu.c is host-side: it includes only <stdint.h> and its own header, and
# takes the GBA region pointers at runtime via ppu_set_memory(). That is what
# lets the same byte-exact rasteriser serve both this port and the RP2350 one.
HOST_SRCS := host/main.c host/video.c host/audio.c host/save.c host/settings.c \
             host/log.c
OBJS      := $(patsubst host/%.c,$(BUILD)/%.o,$(HOST_SRCS)) $(BUILD)/ppu.o

# Must match 3ds/build_objs.sh's ARCH exactly, or the game archive and libctru
# disagree about the ABI and the link is quietly wrong.
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations

# Boot diagnostics: svcOutputDebugString traces (visible in an emulator's log),
# a blue/green splash before AgbMain, and a cycling pillarbox on the top screen
# showing the frame loop is alive. Off by default now that the port boots --
# rebuild with `make -C 3ds CTR_BOOT_DIAG=1` (and pass the same to
# 3ds/build_objs.sh) to bring them back for the next bring-up problem.
# See 3ds/host/trace.h.
CTR_BOOT_DIAG ?= 0

# Regenerated on every make, so the boot log can never report a stale build.
# The git description is what identifies WHICH build; the timestamp is what
# distinguishes two builds of the same tree.
# Read back from the archive build rather than set here: build_objs.sh and this
# Makefile are separate invocations, and a flag passed to only one of them would
# put a binary in your hands whose boot log names the wrong mixer.
CTR_M4A_ASM := $(shell cat $(BUILD)/m4a_asm.flag 2>/dev/null || echo 0)
BUILD_STAMP := $(shell date -u +%Y-%m-%d\ %H:%M:%SZ) $(shell git describe --always --dirty 2>/dev/null || echo nogit)

CFLAGS   := $(ARCH) -g -Wall -O2 -fno-strict-aliasing -fomit-frame-pointer \
            -ffunction-sections -fdata-sections \
            -DCTR_BOOT_DIAG=$(CTR_BOOT_DIAG) -DCTR_M4A_ASM=$(CTR_M4A_ASM) \
            -DCTR_BUILD_STAMP='"$(BUILD_STAMP)"' \
            -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include -I.
LDFLAGS  := $(ARCH) -specs=3dsx.specs -g -Wl,-Map,$(BUILD)/$(TARGET).map

# Point the original mixer's tail jump at itself.
#
# SoundMain (src/m4a_1.s) ends with `ldr r3, lt_SoundMainRAM_Buffer / bx r3`,
# where that literal is `.word SoundMainRAM_Buffer + 1`. On a GBA the buffer is
# an IWRAM copy m4aSoundInit makes, because IWRAM is zero-wait-state.
#
# Here it cannot be: IWRAM is an offset into gGbaMem (3ds/gba_mem.c), an
# ordinary .bss array, and a CXI has no read/write/execute segment to put code
# in. There is also nothing to gain, since the ARM11 runs .text at full speed.
# So the buffer symbol simply IS the routine and src/m4a.c skips the copy.
#
# The symbol is supplied by 3ds/asm/m4a_arm11.s, assembled into the game
# archive. Neither `.set` nor `ld --defsym` can do it: `as` cannot resolve .set
# to a symbol in another object, and --defsym is evaluated before symbol
# resolution, so it produced 0 and the tail jump faulted at address 0. See the
# comment in that file.
LIBS     := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib

MAKEROM  ?= makerom
RSF      := emerald3ds.rsf

.PHONY: all clean cia cci 3dsx

# CIA (install to a console) and CCI/.3ds (boots directly in an emulator).
#
# Deliberately NOT 3dsx. A .3dsx is loaded at a variable address, so every
# absolute pointer needs a load-time relocation, and the 3DSX relocation table
# is indexed in whole 4-byte words. Emerald's script bytecode packs a 4-byte
# pointer straight after a 1-byte opcode (.macro goto, asm/macros/event.inc),
# so thousands of those relocations are unaligned and cannot be encoded at all;
# 3dsxtool aborts with "Unaligned relocation!". A CXI is loaded at a fixed
# address and never relocated, so the same pointers are simply correct.
# See the header comment in emerald3ds.rsf.
all: $(TARGET).cia $(TARGET).3ds

cia: $(TARGET).cia
cci: $(TARGET).3ds

# The SMDH carries the Home-menu title/icon. Optional: if devkitPro's default
# icon is missing, build without one rather than failing.
ifneq ($(wildcard $(APP_ICON)),)
SMDH_DEP  := $(TARGET).smdh
ICON_FLAG := -icon $(TARGET).smdh

$(TARGET).smdh: | $(BUILD)
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@
else
SMDH_DEP  :=
ICON_FLAG :=
endif

$(TARGET).cia: $(TARGET).elf $(SMDH_DEP) $(RSF)
	$(MAKEROM) -f cia -target t -exefslogo -o $@ -elf $(TARGET).elf -rsf $(RSF) $(ICON_FLAG)
	@echo "built ... $@"

$(TARGET).3ds: $(TARGET).elf $(SMDH_DEP) $(RSF)
	$(MAKEROM) -f cci -target t -exefslogo -o $@ -elf $(TARGET).elf -rsf $(RSF) $(ICON_FLAG)
	@echo "built ... $@"

# Kept only so the failure stays reproducible; see the comment on `all`.
3dsx: $(TARGET).elf $(SMDH_DEP)
	$(TOOLS)/3dsxtool $(TARGET).elf $(TARGET).3dsx $(if $(SMDH_DEP),--smdh=$(TARGET).smdh)

$(TARGET).elf: $(OBJS) $(GAME_LIB)
	@echo "linking ... $@"
	$(CC) $(LDFLAGS) $(OBJS) $(GAME_LIB) $(LIBDIRS) $(LIBS) -o $@

$(GAME_LIB):
	@echo "error: $@ missing. Run 3ds/build_objs.sh first." >&2; exit 1

# main.o carries the build stamp, which changes on every invocation, so it has
# to be rebuilt every time. Rebuilding one small file is cheaper than a log line
# that lies about which binary is running.
#
# Done with an always-out-of-date prerequisite, NOT with .PHONY. Marking a real
# file .PHONY tells make it is not a file at all, so make never builds it and
# the link then fails with "cannot find build/main.o". The recipe still comes
# from the pattern rule below; this only adds a prerequisite to it.
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

clean:
	@rm -rf $(BUILD)/*.o $(TARGET).elf $(TARGET).3dsx $(TARGET).3ds $(TARGET).cia \
	        $(TARGET).smdh $(BUILD)/$(TARGET).map
