# Execution Layer (C++) build. Works with the system clang++ (no cmake required).
#
#   make            build the headless targets (selftest, engine_test)
#   make gui        build the terminal GUI (build/terminal)
#   make run-selftest    build + run the live pipeline self-test
#   make engine-test     build + run the threaded engine test
#   make clean
#
# The client talks to the RDB over plain sockets (a small q IPC client), so the
# headless targets have NO third-party dependencies. The GUI additionally needs
# GLFW (brew) and the vendored Dear ImGui + ImPlot (see README).

CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude
BUILD    := build

CORE   := $(BUILD)/oms.o $(BUILD)/risk.o $(BUILD)/matching.o
ENGINE := $(BUILD)/engine.o $(BUILD)/kdb_client.o

# --- GUI (Dear ImGui + ImPlot + GLFW) --------------------------------------
IMGUI_DIR    := third_party/imgui
IMPLOT_DIR   := third_party/implot
GLFW         := $(shell brew --prefix glfw 2>/dev/null)
GUI_INC      := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMPLOT_DIR) -I$(GLFW)/include
GUI_CXXFLAGS := -std=c++20 -O2 -Iinclude $(GUI_INC) -DGL_SILENCE_DEPRECATION
EXT_CXXFLAGS := -std=c++20 -O2 -w $(GUI_INC) -DGL_SILENCE_DEPRECATION
GUI_LDFLAGS  := -L$(GLFW)/lib -lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
EXT_OBJ := $(BUILD)/ext/imgui.o $(BUILD)/ext/imgui_draw.o $(BUILD)/ext/imgui_tables.o \
           $(BUILD)/ext/imgui_widgets.o $(BUILD)/ext/imgui_impl_glfw.o \
           $(BUILD)/ext/imgui_impl_opengl3.o $(BUILD)/ext/implot.o $(BUILD)/ext/implot_items.o

.PHONY: all gui run-selftest engine-test clean
all: $(BUILD)/selftest $(BUILD)/engine_test

$(BUILD)/selftest: $(BUILD)/selftest.o $(BUILD)/kdb_client.o $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/engine_test: $(BUILD)/engine_test.o $(ENGINE) $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

gui: $(BUILD)/terminal
$(BUILD)/terminal: $(BUILD)/terminal_gui.o $(ENGINE) $(CORE) $(EXT_OBJ)
	$(CXX) $(GUI_CXXFLAGS) $^ -o $@ $(GUI_LDFLAGS)

$(BUILD)/terminal_gui.o: src/terminal_gui.cpp | $(BUILD)
	$(CXX) $(GUI_CXXFLAGS) -c $< -o $@

$(BUILD)/ext/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD)/ext
	$(CXX) $(EXT_CXXFLAGS) -c $< -o $@
$(BUILD)/ext/%.o: $(IMGUI_DIR)/backends/%.cpp | $(BUILD)/ext
	$(CXX) $(EXT_CXXFLAGS) -c $< -o $@
$(BUILD)/ext/%.o: $(IMPLOT_DIR)/%.cpp | $(BUILD)/ext
	$(CXX) $(EXT_CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/ext:
	mkdir -p $(BUILD)/ext

run-selftest: $(BUILD)/selftest
	./$(BUILD)/selftest

engine-test: $(BUILD)/engine_test
	./$(BUILD)/engine_test

clean:
	rm -rf $(BUILD)
