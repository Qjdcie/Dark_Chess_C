# =====================================================================
#  Dark Chess (暗棋) — Simplified Makefile (teamwork_v1)
#
#  Targets (Linux/macOS):
#    make              Build the GUI game
#    make clean        Remove build artifacts
#
#  Targets (Windows cross-compile via MinGW-w64):
#    make win          Build dark_chess_gui.exe
#                      Set RAYLIB_MINGW_DIR to your raylib-mingw folder
#                      e.g.:  make win RAYLIB_MINGW_DIR=~/raylib-mingw64
# =====================================================================

CC       = gcc
CFLAGS   = -O3 -Wall -Wextra
LDFLAGS  = -lm
BUILDDIR = build

# Verbose mode: V=1 shows full commands, otherwise quiet
ifeq ($(V),1)
  Q =
else
  Q = @
endif

# Include paths
INCLUDES = -Isrc/core -Isrc/gui

# Raylib flags (statically linked)
RAYLIB_CFLAGS = $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   = $(shell pkg-config --libs raylib 2>/dev/null)

# ---- Windows cross-compile settings (MinGW-w64) ----

WIN_CC      = x86_64-w64-mingw32-gcc
WIN_CFLAGS  = -O3 -march=x86-64 -Wall -Wextra
WIN_LDFLAGS = -lm -static-libgcc

# Raylib for Windows (static, no DLL needed):
# Download raylib-*_win64_mingw-w64 from https://github.com/raysan5/raylib/releases
# or build from source with MinGW. Point RAYLIB_MINGW_DIR at the extracted folder.
RAYLIB_MINGW_DIR ?= $(HOME)/raylib-mingw64
WIN_RAYLIB_CFLAGS = -I$(RAYLIB_MINGW_DIR)/include
WIN_RAYLIB_LIBS   = -L$(RAYLIB_MINGW_DIR)/lib -lraylib \
                    -mwindows -lopengl32 -lgdi32 -lwinmm -static-libgcc

# ---- Source file groups ----

CORE_SRC = src/core/board.c src/core/strategy.c
GUI_SRC  = src/gui/raylib_gui.c

# ---- Targets ----

.PHONY: all gui win clean

all: gui

gui: $(BUILDDIR)/dark_chess_gui

$(BUILDDIR)/dark_chess_gui: src/gui_main.c $(CORE_SRC) $(GUI_SRC)
	@mkdir -p $(BUILDDIR)
	@echo "  [CC] dark_chess_gui"
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) $(RAYLIB_CFLAGS) -o $@ $^ $(LDFLAGS) $(RAYLIB_LIBS)

# ---- Windows cross-compile ----

win: $(BUILDDIR)/dark_chess_gui.exe

$(BUILDDIR)/dark_chess_gui.exe: src/gui_main.c $(CORE_SRC) $(GUI_SRC)
	@mkdir -p $(BUILDDIR)
	@if [ ! -d "$(RAYLIB_MINGW_DIR)/include" ]; then \
	  echo ""; \
	  echo "  ERROR: Raylib for MinGW not found at: $(RAYLIB_MINGW_DIR)"; \
	  echo "  Download raylib-*_win64_mingw-w64 from:"; \
	  echo "    https://github.com/raysan5/raylib/releases"; \
	  echo "  Then run:"; \
	  echo "    make win RAYLIB_MINGW_DIR=<extracted-folder>"; \
	  echo ""; \
	  exit 1; \
	fi
	@echo "  [MINGW] dark_chess_gui.exe (no DLL required)"
	$(Q)$(WIN_CC) $(WIN_CFLAGS) $(INCLUDES) $(WIN_RAYLIB_CFLAGS) -o $@ $^ \
	    $(WIN_LDFLAGS) $(WIN_RAYLIB_LIBS)

clean:
	rm -rf $(BUILDDIR)
