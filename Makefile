# The Minish Cap PC port (see README.md)
#   make pc-windows       tmc_pc.exe + SDL2.dll (MinGW cross compiler, no ROM needed)
#   make pc-dist-windows  the same, packaged as dist/tmc-pc-windows.zip
#   make pc               native build (Linux, 32-bit)
#   make pc-layout        maintainers: regenerate port/assets from baserom.gba (needs `make tools`)
#   make tools            the asset tools pc-layout uses (tools/bin)
.PHONY: default
default: pc-windows

MAKEFLAGS += --no-print-directory

PC_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
PC_SDL_VERSION := 2.30.8
PC_SDL_ROOT := build/sdl2-mingw/SDL2-$(PC_SDL_VERSION)
PC_SDL_DIR := $(PC_SDL_ROOT)/i686-w64-mingw32
PC_SDL_URL := https://github.com/libsdl-org/SDL/releases/download/release-$(PC_SDL_VERSION)/SDL2-devel-$(PC_SDL_VERSION)-mingw.tar.gz
PC_WINDOWS_ARGS = PC_CC=i686-w64-mingw32-gcc PC_AS=i686-w64-mingw32-as \
                  SDL2_CFLAGS="-I$(CURDIR)/$(PC_SDL_DIR)/include/SDL2 -Dmain=SDL_main" \
                  SDL2_LIBS="-L$(CURDIR)/$(PC_SDL_DIR)/lib -lmingw32 -lSDL2main -lSDL2 -mwindows"

.PHONY: pc pc-windows pc-dist-windows pc-layout
pc:
	@$(MAKE) -f pc.mk -j$(PC_JOBS)

pc-windows: $(PC_SDL_DIR)/.extracted
	@$(MAKE) -f pc.mk -j$(PC_JOBS) $(PC_WINDOWS_ARGS)
	cp $(PC_SDL_DIR)/bin/SDL2.dll .

pc-dist-windows: pc-windows
	rm -rf dist/tmc-pc-windows && mkdir -p dist/tmc-pc-windows
	cp tmc_pc.exe SDL2.dll dist/tmc-pc-windows/
	cp port/dist/README.txt dist/tmc-pc-windows/README.txt
	cp $(PC_SDL_ROOT)/README-SDL.txt dist/tmc-pc-windows/
	cp $(PC_SDL_ROOT)/LICENSE.txt dist/tmc-pc-windows/SDL2-LICENSE.txt
	cd dist && rm -f tmc-pc-windows.zip && zip -qr tmc-pc-windows.zip tmc-pc-windows
	@echo "dist/tmc-pc-windows.zip"

$(PC_SDL_DIR)/.extracted:
	mkdir -p build/sdl2-mingw
	curl -L --fail -o build/sdl2-mingw/sdl2.tar.gz $(PC_SDL_URL)
	tar -xzf build/sdl2-mingw/sdl2.tar.gz -C build/sdl2-mingw
	touch $@

pc-layout: tools
	@$(MAKE) -f pc.mk -j$(PC_JOBS) layout

.PHONY: tools
tools: tools/bin

tools/bin:
	mkdir -p tools/cmake-build
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=tools -S tools -B tools/cmake-build
	cmake --build tools/cmake-build -j --target install

.PHONY: clean clean-tools
clean:
	@$(MAKE) -f pc.mk clean
	@$(MAKE) -f pc.mk clean PC_CC=i686-w64-mingw32-gcc

clean-tools:
	rm -rf tools/bin
	rm -rf tools/cmake-build
