# Makefile for the PC port (the top-level Makefile has shortcuts: make pc, pc-windows, pc-dist-windows).
#
# The executable contains no game data. The game's data objects are built from
# port/assets/layout.json.gz (sizes, symbols and pointers only). On the first
# start the game creates a resource pack (tmc_data.pak) from the player's ROM
# and loads the data from it (port/src/resources.c).
#
#   make -f pc.mk           build tmc_pc(.exe), needs no ROM
#   make -f pc.mk layout    regenerate the layout, needs baserom.gba and the decomp tools (make tools)

GAME_VERSION ?= USA
ifneq ($(GAME_VERSION),USA)
$(error the PC port supports GAME_VERSION=USA only)
endif
BUILD_NAME    := tmc
GAME_LANGUAGE := ENGLISH
TRANSLATIONS  := translations/USA.bin
REVISION      := 0

PC_CC ?= gcc
# Windows target: a MinGW compiler, either cross (i686-w64-mingw32-gcc) or native (MSYS2 MINGW32 gcc)
MINGW := $(findstring mingw,$(PC_CC) $(shell $(PC_CC) -dumpmachine 2>/dev/null))
BUILD_DIR := build/pc-$(GAME_VERSION)$(if $(MINGW),-win)

EXE_NAME ?= tmc_pc
EXE_DIR ?= .
EXE := $(EXE_DIR)/$(EXE_NAME)$(if $(MINGW),.exe)

# ---- toolchain ----
# Default: native gcc in 32-bit mode. Windows cross build:
#   make -f pc.mk PC_CC=i686-w64-mingw32-gcc PC_AS=i686-w64-mingw32-as
PC_AS ?= as
PYTHON ?= python3
SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS ?= $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)

M32 := $(if $(MINGW),-mno-ms-bitfields,-m32)
ASM32 := $(if $(MINGW),,--32)
PC_OBJCOPY ?= $(if $(MINGW),$(subst gcc,objcopy,$(PC_CC)),objcopy)

DEFINES := -DPC=1 -DNON_MATCHING=1 -D$(GAME_VERSION) -DREVISION=$(REVISION) -D$(GAME_LANGUAGE)
CPPFLAGS := $(DEFINES) -I include -I port/include -I port/assets/include $(SDL2_CFLAGS)
# The decompiled code relies on GBA/agbcc semantics:
#  - unsigned plain char, wrapping signed overflow, no strict aliasing
#  - data emitted in source order (code indexes across adjacent tables)
#  - locals that are read before being written get a fixed value instead of stack contents
OPT ?= -O2
CFLAGS := $(M32) $(OPT) -g -funsigned-char -fwrapv -fno-strict-aliasing -ftrivial-auto-var-init=zero \
          -fno-toplevel-reorder -fno-pie -malign-data=abi -w -Wno-error
PORT_CFLAGS := $(M32) $(OPT) -g -funsigned-char -fwrapv -fno-strict-aliasing -fno-pie -Wall -Wno-unused-function
# The GBA memory map (0x02000000-0x07FFFFFF) must stay free: move the Windows image above it.
WIN_LDFLAGS := -Wl,--image-base=0x10000000
LDFLAGS := $(M32) $(if $(MINGW),$(WIN_LDFLAGS),-no-pie)
LIBS := $(SDL2_LIBS) -lm

# ---- sources ----
# Game objects in the same order as the GBA linker script (keeps data adjacency).
# (The game's hand written ARM assembly is replaced by C in port/src/asm.)
GAME_OBJS := $(shell grep -vE "\*\w+\.a" linker.ld | grep -oE "(\w|/)+\.o" | awk '!seen[$$0]++')
GAME_C_OBJS := $(filter src/%,$(GAME_OBJS))
GAME_S_OBJS := $(filter-out src/%,$(GAME_OBJS))

PORT_SRCS := $(wildcard port/src/*.c port/src/asm/*.c)
PORT_DATA_OBJS := $(patsubst %.s,%.o,$(wildcard port/data/*.s))
# all objects made from game data, in link order
DATA_OBJS := $(GAME_S_OBJS) $(PORT_DATA_OBJS)

LAYOUT := port/assets/layout.json.gz
SKELETON_DIR := $(BUILD_DIR)/skeleton

OBJS := $(addprefix $(BUILD_DIR)/,$(GAME_C_OBJS)) \
        $(addprefix $(SKELETON_DIR)/,$(DATA_OBJS)) \
        $(patsubst %.c,$(BUILD_DIR)/%.o,$(PORT_SRCS)) \
        $(SKELETON_DIR)/rom_table.o

.SUFFIXES:
.SECONDARY:
.DELETE_ON_ERROR:
.PHONY: all clean layout

all: $(EXE)

clean:
	rm -rf $(BUILD_DIR) $(EXE)

# ---- compile ----
$(BUILD_DIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/port/%.o: port/%.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(CPPFLAGS) $(PORT_CFLAGS) -MMD -MP -c $< -o $@

# ---- game data: zero filled objects with the right layout ----
$(SKELETON_DIR)/.generated: $(LAYOUT) port/tools/gen_asset_skeleton.py
	@mkdir -p $(SKELETON_DIR)/src
	$(PYTHON) port/tools/gen_asset_skeleton.py $(LAYOUT) $(SKELETON_DIR)/src
	touch $@

$(SKELETON_DIR)/%.o: $(SKELETON_DIR)/.generated
	@mkdir -p $(dir $@)
	$(PC_AS) $(ASM32) -o $@ $(SKELETON_DIR)/src/$*.s
ifneq ($(MINGW),)
	@# PE/COFF C symbols have a leading underscore, the assembly data does not
	$(PC_OBJCOPY) --prefix-symbols=_ $@
endif

# ---- link ----
$(BUILD_DIR)/linker.i: linker.ld
	@mkdir -p $(dir $@)
	$(PC_CC) -E -P -x c $(DEFINES) $< -o $@

$(BUILD_DIR)/ram_symbols.ld: $(BUILD_DIR)/linker.i port/tools/gen_ram_syms.py
	$(PYTHON) port/tools/gen_ram_syms.py $< $@ $(if $(MINGW),--prefix _)

$(EXE): $(OBJS) $(BUILD_DIR)/ram_symbols.ld
	$(PC_CC) $(LDFLAGS) -o $@ $(OBJS) $(BUILD_DIR)/ram_symbols.ld $(LIBS)

# ---- layout (maintainers only: needs baserom.gba and `make tools`) ----
# The data files are assembled for real (as 32-bit ELF objects, whatever the
# target) and only their layout is kept.
LAYOUT_DIR := build/pc-layout
LAYOUT_AS ?= as
PREPROC := tools/bin/preproc
ASSET_PROCESSOR := tools/bin/asset_processor
ENUM_PROCESSOR := tools/extract_include_enum.py
ASSETS_DIR := $(LAYOUT_DIR)/assets
ENUM_DIR := $(LAYOUT_DIR)/enum_include
ENUM_ASM_HEADERS := $(patsubst include/%.h,$(ENUM_DIR)/%.inc,$(wildcard include/*.h))
LAYOUT_ASFLAGS := --32 --divide --defsym $(GAME_VERSION)=1 --defsym REVISION=$(REVISION) \
                  --defsym $(GAME_LANGUAGE)=1 --defsym PC=1 -I . -I $(ASSETS_DIR) -I $(ENUM_DIR)

layout: $(addprefix $(LAYOUT_DIR)/,$(DATA_OBJS)) port/tools/make_asset_layout.py
	@mkdir -p port/assets/include/assets
	$(PYTHON) port/tools/make_asset_layout.py baserom.gba $(LAYOUT) $(LAYOUT_DIR) $(DATA_OBJS)
	@mkdir -p port/assets/include/assets
	cp $(ASSETS_DIR)/gfx_offsets.h $(ASSETS_DIR)/map_offsets.h port/assets/include/assets/

$(LAYOUT_DIR)/extracted_assets: assets/assets.json assets/gfx.json assets/map.json assets/samples.json \
                                assets/sounds.json $(TRANSLATIONS)
	@mkdir -p $(LAYOUT_DIR)
	$(ASSET_PROCESSOR) extract $(GAME_VERSION) $(ASSETS_DIR)
	@# The raw extracted assets contain absolute ROM pointers (songs, ...). Converting
	@# them to sources and rebuilding (the decomp's "custom" build) makes them relocatable.
	$(ASSET_PROCESSOR) convert $(GAME_VERSION) $(ASSETS_DIR)
	$(ASSET_PROCESSOR) build $(GAME_VERSION) $(ASSETS_DIR)
	touch $@

translations/%.bin: translations/%.json
	tools/bin/tmc_strings -p --source $< --dest $@

$(ENUM_DIR)/%.inc: include/%.h
	@mkdir -p $(dir $@)
	$(PYTHON) $(ENUM_PROCESSOR) $< gcc "-D__attribute__(x)=" "-D$(GAME_VERSION)" "-E" "-nostdinc" \
	  "-Iport/tools/fake_libc" "-iquote include" > $@

$(LAYOUT_DIR)/%.o: %.s $(ENUM_ASM_HEADERS) $(LAYOUT_DIR)/extracted_assets port/tools/asfilter.py
	@mkdir -p $(dir $@)
	$(PREPROC) $(BUILD_NAME) $< -- -I $(ASSETS_DIR) -I $(ENUM_DIR) | \
	  $(PYTHON) port/tools/asfilter.py -I $(ASSETS_DIR) -I $(ENUM_DIR) > $(LAYOUT_DIR)/$*.pc.s
	$(LAYOUT_AS) $(LAYOUT_ASFLAGS) -o $@ $(LAYOUT_DIR)/$*.pc.s

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
