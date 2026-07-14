# Execution Layer (C++) build.
# Works with the system clang++ (no cmake required).
#
#   make            # build both binaries
#   make demo       # build+run the offline MA-cross demo (build/execution)
#   make selftest   # build the live pipeline self-test (build/selftest)
#   make run-selftest   # build+run the self-test (needs the KDB+ stack up)
#   make clean
#
# The self-test talks to the RDB over plain sockets (pure-socket q IPC client),
# so there is NO KX SDK / libq dependency.

CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude
BUILD    := build

# Shared engine objects
CORE := $(BUILD)/oms.o $(BUILD)/risk.o $(BUILD)/matching.o
# Offline demo (MA-cross auto-trader)
DEMO := $(BUILD)/main.o $(BUILD)/strategy.o $(BUILD)/market_data.o
# Live self-test (native KDB+ client)
SELF := $(BUILD)/selftest.o $(BUILD)/kdb_client.o
# Threaded engine (shared by engine_test and the GUI)
ENGINE := $(BUILD)/engine.o $(BUILD)/kdb_client.o

# --- GUI (Dear ImGui + ImPlot + GLFW) --------------------------------------
IMGUI_DIR  := third_party/imgui
IMPLOT_DIR := third_party/implot
GLFW       := $(shell brew --prefix glfw 2>/dev/null)
GUI_INC    := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMPLOT_DIR) -I$(GLFW)/include
GUI_CXXFLAGS := -std=c++20 -O2 -Iinclude $(GUI_INC) -DGL_SILENCE_DEPRECATION
EXT_CXXFLAGS := -std=c++20 -O2 -w $(GUI_INC) -DGL_SILENCE_DEPRECATION
GUI_LDFLAGS := -L$(GLFW)/lib -lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
EXT_OBJ := $(BUILD)/ext/imgui.o $(BUILD)/ext/imgui_draw.o $(BUILD)/ext/imgui_tables.o \
           $(BUILD)/ext/imgui_widgets.o $(BUILD)/ext/imgui_impl_glfw.o \
           $(BUILD)/ext/imgui_impl_opengl3.o $(BUILD)/ext/implot.o $(BUILD)/ext/implot_items.o

.PHONY: all demo selftest run-selftest engine-test gui clean
all: $(BUILD)/execution $(BUILD)/selftest $(BUILD)/engine_test

$(BUILD)/execution: $(DEMO) $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/selftest: $(SELF) $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/engine_test: $(BUILD)/engine_test.o $(ENGINE) $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

# GUI executable (kept out of `all` so headless targets don't need GLFW).
gui: $(BUILD)/terminal
$(BUILD)/terminal: $(BUILD)/terminal_gui.o $(ENGINE) $(CORE) $(EXT_OBJ)
	$(CXX) $(GUI_CXXFLAGS) $^ -o $@ $(GUI_LDFLAGS)

# terminal_gui.cpp needs the imgui/glfw include paths.
$(BUILD)/terminal_gui.o: src/terminal_gui.cpp | $(BUILD)
	$(CXX) $(GUI_CXXFLAGS) -c $< -o $@

# Vendored imgui/implot objects (warnings silenced; not our code).
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

demo: $(BUILD)/execution
	./$(BUILD)/execution

selftest: $(BUILD)/selftest

run-selftest: $(BUILD)/selftest
	./$(BUILD)/selftest

engine-test: $(BUILD)/engine_test
	./$(BUILD)/engine_test

clean:
	rm -rf $(BUILD)
