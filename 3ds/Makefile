# Host half of the 3DS port: links the ARM11 game archive against libctru.
#
# Build order from the repo root:
#   make tools && make generated              # decomp tools + generated headers
#   python3 tools/generate_wasm_assets.py     # -> build/assets
#   rp2350/gen_sound_assets.sh                # one-time: wav->bin, mid->song .s
#   3ds/build_objs.sh                         # -> 3ds/build/libpokeemerald.a
#   make -C 3ds                               # -> 3ds/emerald3ds.3dsx
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
HOST_SRCS := host/main.c host/video.c host/audio.c host/save.c
OBJS      := $(patsubst host/%.c,$(BUILD)/%.o,$(HOST_SRCS)) $(BUILD)/ppu.o

# Must match 3ds/build_objs.sh's ARCH exactly, or the game archive and libctru
# disagree about the ABI and the link is quietly wrong.
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations

CFLAGS   := $(ARCH) -g -Wall -O2 -fno-strict-aliasing -fomit-frame-pointer \
            -ffunction-sections -fdata-sections \
            -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include -I.
LDFLAGS  := $(ARCH) -specs=3dsx.specs -g -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS     := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib

.PHONY: all clean
all: $(TARGET).3dsx

# The SMDH carries the Home-menu title/icon. Optional: if devkitPro's default
# icon is missing, ship a bare 3dsx rather than failing the build.
ifneq ($(wildcard $(APP_ICON)),)
$(TARGET).smdh: | $(BUILD)
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	$(TOOLS)/3dsxtool $< $@ --smdh=$(TARGET).smdh
	@echo "built ... $@"
else
$(TARGET).3dsx: $(TARGET).elf
	$(TOOLS)/3dsxtool $< $@
	@echo "built ... $@ (no SMDH: $(APP_ICON) missing)"
endif

$(TARGET).elf: $(OBJS) $(GAME_LIB)
	@echo "linking ... $@"
	$(CC) $(LDFLAGS) $(OBJS) $(GAME_LIB) $(LIBDIRS) $(LIBS) -o $@

$(GAME_LIB):
	@echo "error: $@ missing. Run 3ds/build_objs.sh first." >&2; exit 1

$(BUILD)/%.o: host/%.c | $(BUILD)
	@echo "compiling ... $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ppu.o: ../rp2350/ppu.c | $(BUILD)
	@echo "compiling ... $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD)/*.o $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh $(BUILD)/$(TARGET).map
