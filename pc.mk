# Makefile for the native PC port.
#
#   make -f pc.mk            (or: make pc)
#
# Needs: a 32-bit capable host C compiler (gcc -m32 / i686 mingw), GNU as for
# the same target, python3 + pycparser, SDL2 (32-bit) and a legally obtained
# ROM (baserom.gba for USA) to extract the assets from. See PC_PORT.md.

GAME_VERSION ?= USA
BUILD_DIR := build/pc-$(GAME_VERSION)

ifeq ($(GAME_VERSION), EU)
BUILD_NAME    := tmc_eu
GAME_LANGUAGE := ENGLISH
TRANSLATIONS  := translations/English.bin translations/French.bin translations/German.bin translations/Spanish.bin translations/Italian.bin
else ifeq ($(GAME_VERSION), JP)
BUILD_NAME    := tmc_jp
GAME_LANGUAGE := JAPANESE
TRANSLATIONS  :=
else ifeq ($(GAME_VERSION), USA)
BUILD_NAME    := tmc
GAME_LANGUAGE := ENGLISH
TRANSLATIONS  := translations/USA.bin
else
$(error the PC port supports GAME_VERSION=USA, EU or JP)
endif
REVISION := 0

EXE_NAME ?= tmc_pc
ifeq ($(OS),Windows_NT)
EXE := $(EXE_NAME).exe
else
EXE := $(EXE_NAME)
endif

# ---- toolchain ----
# Default: native gcc in 32-bit mode. For Windows cross builds use e.g.
#   make -f pc.mk PC_CC=i686-w64-mingw32-gcc PC_AS=i686-w64-mingw32-as
PC_CC ?= gcc
PC_AS ?= as
PYTHON ?= python3
SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS ?= $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)

M32 := $(if $(findstring mingw,$(PC_CC)),,-m32)
ASM32 := $(if $(findstring mingw,$(PC_AS)),,--32)

PREPROC := tools/bin/preproc
ASSET_PROCESSOR := tools/bin/asset_processor
ENUM_PROCESSOR := tools/extract_include_enum.py

ASSETS_DIR := $(BUILD_DIR)/assets
ENUM_DIR := $(BUILD_DIR)/enum_include

DEFINES := -DPC=1 -DNON_MATCHING=1 -D$(GAME_VERSION) -DREVISION=$(REVISION) -D$(GAME_LANGUAGE)
CPPFLAGS := $(DEFINES) -I include -I port/include -I $(BUILD_DIR) $(SDL2_CFLAGS)
# The decompiled code relies on GBA/agbcc semantics:
#  - unsigned plain char, wrapping signed overflow, no strict aliasing
#  - data emitted in source order (code indexes across adjacent tables)
OPT ?= -O2
CFLAGS := $(M32) $(OPT) -g -funsigned-char -fwrapv -fno-strict-aliasing -fno-toplevel-reorder \
          -fno-pie -malign-data=abi \
          -w -Wno-error
PORT_CFLAGS := $(M32) $(OPT) -g -funsigned-char -fwrapv -fno-strict-aliasing -fno-pie -Wall -Wno-unused-function
ASFLAGS := $(ASM32) --defsym $(GAME_VERSION)=1 --defsym REVISION=$(REVISION) --defsym $(GAME_LANGUAGE)=1 --defsym PC=1 \
           -I . -I $(ASSETS_DIR) -I $(ENUM_DIR)
LDFLAGS := $(M32) -no-pie
LIBS := $(SDL2_LIBS) -lm

# ---- sources ----
# Game objects in the same order as the GBA linker script (keeps data adjacency).
LD_OBJS := $(shell grep -vE "\*\w+\.a" linker.ld | grep -oE "(\w|/)+\.o" | awk '!seen[$$0]++')
# Hand written ARM assembly is replaced by C in port/src/asm.
EXCLUDED_OBJS := asm/src/crt0.o asm/src/stack_check.o asm/src/veneer.o asm/src/code_08000E44.o \
                 asm/lib/libgcc.o asm/src/code_08000F10.o asm/src/enemy.o asm/src/code_08001A7C.o \
                 asm/src/code_08003FC4.o asm/src/code_080043E8.o asm/src/code_08007CAC.o asm/src/player.o \
                 asm/src/script.o asm/src/projectileUpdate.o asm/src/intr.o asm/lib/libagbsyscall.o \
                 asm/lib/m4a_asm.o src/eeprom.o
GAME_OBJS := $(filter-out $(EXCLUDED_OBJS),$(LD_OBJS))
GAME_C_OBJS := $(filter src/%,$(GAME_OBJS))
GAME_S_OBJS := $(filter-out src/%,$(GAME_OBJS))

PORT_SRCS := $(wildcard port/src/*.c port/src/asm/*.c)
PORT_DATA_SRCS := $(wildcard port/data/*.s)

OBJS := $(addprefix $(BUILD_DIR)/,$(GAME_C_OBJS) $(GAME_S_OBJS)) \
        $(patsubst %.c,$(BUILD_DIR)/%.o,$(PORT_SRCS)) \
        $(patsubst %.s,$(BUILD_DIR)/%.o,$(PORT_DATA_SRCS))

ENUM_ASM_SRCS := $(wildcard include/*.h)
ENUM_ASM_HEADERS := $(patsubst include/%.h,$(ENUM_DIR)/%.inc,$(ENUM_ASM_SRCS))

.SUFFIXES:
.SECONDARY:
.DELETE_ON_ERROR:
.PHONY: all clean assets

all: $(EXE)

clean:
	rm -rf $(BUILD_DIR) $(EXE)

# ---- assets ----
$(BUILD_DIR)/extracted_assets: assets/assets.json assets/gfx.json assets/map.json assets/samples.json assets/sounds.json $(TRANSLATIONS)
	@mkdir -p $(BUILD_DIR)
	$(ASSET_PROCESSOR) extract $(GAME_VERSION) $(ASSETS_DIR)
	touch $@

assets: $(BUILD_DIR)/extracted_assets

translations/%.bin: translations/%.json
	tools/bin/tmc_strings -p --source $< --dest $@

$(ENUM_DIR)/%.inc: include/%.h
	@mkdir -p $(dir $@)
	$(PYTHON) $(ENUM_PROCESSOR) $< $(PC_CC) "-D__attribute__(x)=" "-D$(GAME_VERSION)" "-E" "-nostdinc" "-Iport/tools/fake_libc" "-iquote include" > $@

# ---- compile ----
$(BUILD_DIR)/src/%.o: src/%.c | $(BUILD_DIR)/extracted_assets
	@mkdir -p $(dir $@)
	$(PC_CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/port/%.o: port/%.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(CPPFLAGS) $(PORT_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.s $(ENUM_ASM_HEADERS) $(BUILD_DIR)/extracted_assets port/tools/asfilter.py
	@mkdir -p $(dir $@)
	$(PREPROC) $(BUILD_NAME) $< -- -I $(ASSETS_DIR) -I $(ENUM_DIR) | \
	  $(PYTHON) port/tools/asfilter.py -I $(ASSETS_DIR) -I $(ENUM_DIR) > $(BUILD_DIR)/$*.pc.s
	$(PC_AS) $(ASFLAGS) -o $@ $(BUILD_DIR)/$*.pc.s

# ---- link ----
$(BUILD_DIR)/linker.i: linker.ld
	@mkdir -p $(dir $@)
	$(PC_CC) -E -P -x c $(DEFINES) $< -o $@

$(BUILD_DIR)/ram_symbols.ld: $(BUILD_DIR)/linker.i port/tools/gen_ram_syms.py
	$(PYTHON) port/tools/gen_ram_syms.py $< $@

$(EXE): $(OBJS) $(BUILD_DIR)/ram_symbols.ld
	$(PC_CC) $(LDFLAGS) -o $@ $(OBJS) $(BUILD_DIR)/ram_symbols.ld $(LIBS)

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
