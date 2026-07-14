// ============================================================================
// terminal_gui.cpp -- Execution Layer terminal (Dear ImGui + ImPlot + GLFW).
//
// A single OS window with tiled/docked panels, Bloomberg-terminal styling:
//   * Account bar   -- cash / equity / PnL / status / kill switch
//   * Market Watch  -- live top-of-book for every watched symbol (+ add ticker)
//   * Order Ticket  -- pick a symbol, enter $ notional, BUY / SELL / FLATTEN
//   * Positions     -- open positions with live unrealized PnL (+ per-row flatten)
//   * Event Log     -- fills, rejects, connection events
//
// All trading state lives on the TradingEngine's background thread; this file is
// pure presentation: it reads an EngineView snapshot each frame and posts
// Commands. See engine.hpp for the threading model.
// ============================================================================
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder for the default tiled layout
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>

#include "execution/engine.hpp"

using namespace el;

namespace {

// --- Bloomberg-ish palette -------------------------------------------------
const ImVec4 kBg      = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
const ImVec4 kPanel   = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
const ImVec4 kAmber   = ImVec4(1.00f, 0.65f, 0.00f, 1.00f);
const ImVec4 kAmberDim= ImVec4(0.55f, 0.36f, 0.00f, 1.00f);
const ImVec4 kText    = ImVec4(0.90f, 0.90f, 0.85f, 1.00f);
const ImVec4 kGreen   = ImVec4(0.20f, 0.85f, 0.35f, 1.00f);
const ImVec4 kRed     = ImVec4(0.95f, 0.25f, 0.25f, 1.00f);
const ImVec4 kCyan    = ImVec4(0.20f, 0.75f, 0.85f, 1.00f);

ImVec4 pnl_color(double v) { return v >= 0 ? kGreen : kRed; }

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.Colors[ImGuiCol_WindowBg] = kBg;
    s.Colors[ImGuiCol_ChildBg] = kPanel;
    s.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.11f, 0.02f, 1.0f);
    s.Colors[ImGuiCol_Text] = kText;
    s.Colors[ImGuiCol_Header] = kAmberDim;
    s.Colors[ImGuiCol_HeaderHovered] = kAmber;
    s.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.13f, 0.05f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered] = kAmberDim;
    s.Colors[ImGuiCol_ButtonActive] = kAmber;
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.10f, 0.04f, 1.0f);
    s.Colors[ImGuiCol_TableRowBg] = kPanel;
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
}

// One-time tiled layout: split the dockspace into the classic terminal quadrants.
void build_default_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID right, bottom, left = dockspace_id;
    right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.30f, nullptr, &left);
    bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.32f, nullptr, &left);

    ImGui::DockBuilderDockWindow("Market Watch", left);
    ImGui::DockBuilderDockWindow("Positions", bottom);
    ImGui::DockBuilderDockWindow("Order Ticket", right);
    ImGui::DockBuilderDockWindow("Event Log", right);
    ImGui::DockBuilderFinish(dockspace_id);
}

void money_text(const char* label, double v, ImVec4 col) {
    ImGui::TextColored(kCyan, "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(col, "%s%.2f", v >= 0 ? " " : "", v);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : 5011;

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow* window = glfwCreateWindow(1440, 900, "Execution Layer // Terminal", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "window creation failed (no display?)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // --- app state -----------------------------------------------------------
    TradingEngine* engine = nullptr;
    bool started = false;
    bool layout_built = false;
    float capital_input = 10'000.0f;
    char newsym[32] = "";
    int sel_sym = 0;
    float order_notional = 1'000.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!started) {
            // Centered funding dialog before the desk goes live.
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Fund Account", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
            ImGui::TextColored(kAmber, "EXECUTION LAYER");
            ImGui::TextColored(kText, "Paper trading on live Coinbase market data.");
            ImGui::Separator();
            ImGui::TextColored(kCyan, "Initial capital ($)");
            ImGui::InputFloat("##cap", &capital_input, 1000.0f, 10000.0f, "%.2f");
            if (capital_input < 0) capital_input = 0;
            ImGui::Spacing();
            if (ImGui::Button("START DESK", ImVec2(180, 30))) {
                RiskLimits lim;
                lim.max_order_qty = 100.0;
                lim.max_position_qty = 1e6;
                lim.max_order_notional = capital_input;  // no single order beyond funded capital
                engine = new TradingEngine(host, port, {"BTC-USD", "ETH-USD"}, capital_input, lim);
                engine->start();
                started = true;
            }
            ImGui::End();
        } else {
            EngineView v = engine->view();

            // Full-window dockspace host.
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##root", nullptr, host_flags);
            ImGui::PopStyleVar(2);

            // Account bar (menu bar).
            if (ImGui::BeginMenuBar()) {
                ImGui::TextColored(kAmber, "EXECUTION LAYER");
                ImGui::Separator();
                ImGui::TextColored(v.connected ? kGreen : kRed, "%s", v.status.c_str());
                ImGui::Separator();
                money_text("CASH", v.cash, kText);
                ImGui::Separator();
                money_text("EQUITY", v.equity, kText);
                ImGui::Separator();
                money_text("PnL", v.total_pnl, pnl_color(v.total_pnl));
                ImGui::Separator();
                if (v.risk_killed) ImGui::TextColored(kRed, "[HALTED]");
                else if (ImGui::SmallButton("KILL")) engine->post({Command::Kill, "", 0, false});
                ImGui::EndMenuBar();
            }

            ImGuiID dockspace_id = ImGui::GetID("DockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
            if (!layout_built) { build_default_layout(dockspace_id); layout_built = true; }
            ImGui::End();

            // --- Market Watch ---------------------------------------------------
            ImGui::Begin("Market Watch");
            ImGui::SetNextItemWidth(140);
            ImGui::InputTextWithHint("##newsym", "e.g. SOL-USD", newsym, sizeof(newsym));
            ImGui::SameLine();
            if (ImGui::Button("+ Watch") && newsym[0]) {
                for (char* p = newsym; *p; ++p) *p = (char)toupper(*p);
                engine->post({Command::AddSymbol, newsym, 0, false});
                newsym[0] = '\0';
            }
            if (ImGui::BeginTable("mw", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("SYMBOL");
                ImGui::TableSetupColumn("BID");
                ImGui::TableSetupColumn("ASK");
                ImGui::TableSetupColumn("MID");
                ImGui::TableSetupColumn("SPREAD");
                ImGui::TableHeadersRow();
                for (const auto& s : v.symbols) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(kAmber, "%s", s.symbol.c_str());
                    if (s.has_quote) {
                        ImGui::TableNextColumn(); ImGui::TextColored(kGreen, "%.2f", s.bid);
                        ImGui::TableNextColumn(); ImGui::TextColored(kRed, "%.2f", s.ask);
                        ImGui::TableNextColumn(); ImGui::Text("%.2f", s.mid);
                        ImGui::TableNextColumn(); ImGui::Text("%.2f", s.ask - s.bid);
                    } else {
                        ImGui::TableNextColumn(); ImGui::TextDisabled("...");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("...");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("...");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("...");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();

            // --- Order Ticket ---------------------------------------------------
            ImGui::Begin("Order Ticket");
            std::vector<const char*> names;
            for (const auto& s : v.symbols) names.push_back(s.symbol.c_str());
            if (!names.empty()) {
                if (sel_sym >= (int)names.size()) sel_sym = 0;
                ImGui::TextColored(kCyan, "Symbol");
                ImGui::Combo("##sym", &sel_sym, names.data(), (int)names.size());
                ImGui::TextColored(kCyan, "Notional ($)");
                ImGui::InputFloat("##notional", &order_notional, 100.0f, 1000.0f, "%.2f");
                if (order_notional < 0) order_notional = 0;
                const std::string sym = names[sel_sym];
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.35f, 0.15f, 1.0f));
                if (ImGui::Button("BUY", ImVec2(90, 34)))
                    engine->post({Command::Buy, sym, order_notional, true});
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.12f, 0.12f, 1.0f));
                if (ImGui::Button("SELL", ImVec2(90, 34)))
                    engine->post({Command::Sell, sym, order_notional, true});
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("FLATTEN", ImVec2(90, 34)))
                    engine->post({Command::Flatten, sym, 0, false});
            } else {
                ImGui::TextDisabled("no symbols watched");
            }
            ImGui::End();

            // --- Positions ------------------------------------------------------
            ImGui::Begin("Positions");
            if (ImGui::BeginTable("pos", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("SYMBOL");
                ImGui::TableSetupColumn("NET");
                ImGui::TableSetupColumn("AVG PX");
                ImGui::TableSetupColumn("UNREAL");
                ImGui::TableSetupColumn("REAL");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (const auto& s : v.symbols) {
                    if (s.net_qty == 0.0 && s.realized == 0.0) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(kAmber, "%s", s.symbol.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%.6f", s.net_qty);
                    ImGui::TableNextColumn(); ImGui::Text("%.2f", s.avg_price);
                    ImGui::TableNextColumn(); ImGui::TextColored(pnl_color(s.unrealized), "%+.2f", s.unrealized);
                    ImGui::TableNextColumn(); ImGui::TextColored(pnl_color(s.realized), "%+.2f", s.realized);
                    ImGui::TableNextColumn();
                    if (s.net_qty != 0.0) {
                        ImGui::PushID(s.symbol.c_str());
                        if (ImGui::SmallButton("flatten")) engine->post({Command::Flatten, s.symbol, 0, false});
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();

            // --- Event Log ------------------------------------------------------
            ImGui::Begin("Event Log");
            for (auto it = v.log.rbegin(); it != v.log.rend(); ++it)
                ImGui::TextWrapped("%s", it->c_str());
            ImGui::End();
        }

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(kBg.x, kBg.y, kBg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (engine) { engine->stop(); delete engine; }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
