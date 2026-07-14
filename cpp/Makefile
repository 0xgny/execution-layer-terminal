# Execution Layer (C++) build. Works with the system clang++ (no cmake required).
#
#   make            build the headless targets (selftest, engine_test)
#   make gui        build the terminal GUI (build/terminal)
#   make run-selftest    build + run the live pipeline self-test
#   make engine-test     build + run the threaded engine test
#   make alpaca-test     build + run the Alpaca stock-data smoke test
#   make clean
#
# The KDB+ client talks to the RDB over plain sockets (a small q IPC client);
# `selftest` needs nothing beyond the system compiler. `engine_test` and `gui`
# additionally pull in AlpacaClient (stock market data), which needs libcurl
# (system-provided on macOS/Linux) and the vendored nlohmann/json single header
# (see README). The GUI further needs GLFW (brew) and vendored Dear ImGui +
# ImPlot (see README).

CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude -Ithird_party/json -MMD -MP
BUILD    := build

# libcurl backs AlpacaClient (stock market data). System/Homebrew curl both
# provide a plain -lcurl; no extra include path is needed on macOS or Linux.
CURL_LDFLAGS := -lcurl

CORE   := $(BUILD)/oms.o $(BUILD)/risk.o $(BUILD)/matching.o
ENGINE := $(BUILD)/engine.o $(BUILD)/kdb_client.o $(BUILD)/alpaca_client.o $(BUILD)/stock_feed.o

# --- GUI (Dear ImGui + ImPlot + GLFW) --------------------------------------
IMGUI_DIR    := third_party/imgui
IMPLOT_DIR   := third_party/implot
GLFW         := $(shell brew --prefix glfw 2>/dev/null)
GUI_INC      := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMPLOT_DIR) -I$(GLFW)/include
GUI_CXXFLAGS := -std=c++20 -O2 -Iinclude -Ithird_party/json $(GUI_INC) -DGL_SILENCE_DEPRECATION -MMD -MP
EXT_CXXFLAGS := -std=c++20 -O2 -w $(GUI_INC) -DGL_SILENCE_DEPRECATION
GUI_LDFLAGS  := -L$(GLFW)/lib -lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
EXT_OBJ := $(BUILD)/ext/imgui.o $(BUILD)/ext/imgui_draw.o $(BUILD)/ext/imgui_tables.o \
           $(BUILD)/ext/imgui_widgets.o $(BUILD)/ext/imgui_impl_glfw.o \
           $(BUILD)/ext/imgui_impl_opengl3.o $(BUILD)/ext/implot.o $(BUILD)/ext/implot_items.o

.PHONY: all gui run-selftest engine-test alpaca-test clean
all: $(BUILD)/selftest $(BUILD)/engine_test

$(BUILD)/selftest: $(BUILD)/selftest.o $(BUILD)/kdb_client.o $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/engine_test: $(BUILD)/engine_test.o $(ENGINE) $(CORE)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(CURL_LDFLAGS)

$(BUILD)/alpaca_test: $(BUILD)/alpaca_test.o $(BUILD)/alpaca_client.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(CURL_LDFLAGS)

gui: $(BUILD)/terminal
$(BUILD)/terminal: $(BUILD)/terminal_gui.o $(ENGINE) $(CORE) $(EXT_OBJ)
	$(CXX) $(GUI_CXXFLAGS) $^ -o $@ $(GUI_LDFLAGS) $(CURL_LDFLAGS)

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

alpaca-test: $(BUILD)/alpaca_test
	./$(BUILD)/alpaca_test

clean:
	rm -rf $(BUILD)

# Auto-generated header dependencies (-MMD -MP above) so editing a .hpp alone
# triggers recompiles of every .cpp that includes it, not just its own .cpp --
# without this, `make` can silently link stale .o files against a changed
# struct layout (undefined behavior, sometimes a heap-corruption crash).
-include $(wildcard $(BUILD)/*.d) $(wildcard $(BUILD)/ext/*.d)
