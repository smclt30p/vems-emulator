# VEMS Emulator — Dear ImGui (SDL2 + OpenGL3) front-end around simavr.

SIMAVR   := vendor/simavr
SIMHOST  := $(shell clang -dumpmachine)
SIMOBJ   := $(SIMAVR)/simavr/obj-$(SIMHOST)
LIBSIMAVR:= $(SIMOBJ)/libsimavr.a
LIBELF   := $(firstword $(wildcard /usr/local/Cellar/libelf/*/))

IMGUI    := vendor/imgui
SRC      := src

CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -MMD -MP

SDL_CFLAGS := $(shell PKG_CONFIG_PATH=$(shell brew --prefix sdl2)/lib/pkgconfig pkg-config --cflags sdl2)
SDL_LIBS   := $(shell PKG_CONFIG_PATH=$(shell brew --prefix sdl2)/lib/pkgconfig pkg-config --libs sdl2)

INCLUDES := -I$(SRC) -I$(IMGUI) -I$(IMGUI)/backends \
            -I$(SIMAVR)/simavr/sim \
            -I$(SIMAVR)/examples/parts \
            -I$(LIBELF)include -I$(LIBELF)include/libelf \
            $(SDL_CFLAGS)

LDLIBS   := $(LIBSIMAVR) $(LIBELF)lib/libelf.a $(SDL_LIBS) \
            -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lpthread

IMGUI_SRC := $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
             $(IMGUI)/imgui_widgets.cpp \
             $(IMGUI)/backends/imgui_impl_sdl2.cpp $(IMGUI)/backends/imgui_impl_opengl3.cpp

APP_SRC  := $(SRC)/main.cpp $(SRC)/emu_core.cpp $(SRC)/uart_bridge.cpp $(SRC)/mcp3208.cpp $(SRC)/hip9011.cpp $(SRC)/outputs.cpp $(SRC)/hc259.cpp

BUILD    := build
OBJ      := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(IMGUI_SRC) $(APP_SRC)))
DEPS     := $(OBJ:.o=.d)

TARGET   := vems_emulator

VPATH := $(IMGUI):$(IMGUI)/backends:$(SRC)

all: $(TARGET)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJ) $(LIBSIMAVR)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDLIBS) -o $@

# Apply the VEMS patch (flash counters + libelf paths) to the pristine simavr
# submodule, then build libsimavr.a. Patch step is idempotent.
SIMPATCH := $(CURDIR)/patches/simavr-vems.patch

$(LIBSIMAVR): | simavr
simavr:
	@test -e $(SIMAVR)/simavr/sim/sim_avr.h || \
		{ echo "simavr submodule missing — run: git submodule update --init --recursive"; exit 1; }
	@cd $(SIMAVR) && if git apply --reverse --check $(SIMPATCH) 2>/dev/null; then \
		echo "simavr: VEMS patch already applied"; \
	else git apply $(SIMPATCH) && echo "simavr: applied VEMS patch"; fi
	$(MAKE) -C $(SIMAVR)/simavr

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(DEPS)

.PHONY: all clean simavr
