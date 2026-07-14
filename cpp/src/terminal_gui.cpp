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
#include <algorithm>
#include <cctype>
#include <cmath>
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

// Format a number with thousands separators, e.g. 62558.87 -> "62,558.87".
std::string commafy(double v, int dec = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
    std::string s = buf;
    std::size_t dot = s.find('.');
    int end = (dot == std::string::npos) ? (int)s.size() : (int)dot;
    int start = (!s.empty() && s[0] == '-') ? 1 : 0;
    for (int i = end - 3; i > start; i -= 3) s.insert((std::size_t)i, ",");
    return s;
}

// Same, but with an explicit leading '+' for non-negative values (for PnL).
std::string signed_money(double v, int dec = 2) {
    return (v >= 0 ? "+" : "") + commafy(v, dec);
}

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

    // Layout: [ left: watch/search ] [ center: price chart / pnl ] [ right: ticket / positions / log ]
    ImGuiID center = dockspace_id, right, left, center_bottom, right_bottom;
    right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
    center_bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.38f, nullptr, &center);
    right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Market Watch", left);
    ImGui::DockBuilderDockWindow("Ticker Search", left);
    ImGui::DockBuilderDockWindow("Price Chart", center);
    ImGui::DockBuilderDockWindow("PnL", center_bottom);
    ImGui::DockBuilderDockWindow("Order Ticket", right);
    ImGui::DockBuilderDockWindow("Positions", right_bottom);
    ImGui::DockBuilderDockWindow("Event Log", right_bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

void money_text(const char* label, double v, ImVec4 col) {
    ImGui::TextColored(kCyan, "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(col, "%s", commafy(v).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : 5011;       // RDB
    const int tp_port = argc > 3 ? std::atoi(argv[3]) : 5010;    // tickerplant

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
                engine = new TradingEngine(host, port, tp_port, {"BTC-USD", "ETH-USD"}, capital_input, lim);
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
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                    if (ImGui::Selectable(s.symbol.c_str(), s.symbol == v.focus,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        engine->post({Command::SetFocus, s.symbol, 0, false});
                    ImGui::PopStyleColor();
                    if (s.has_quote) {
                        ImGui::TableNextColumn(); ImGui::TextColored(kGreen, "%s", commafy(s.bid).c_str());
                        ImGui::TableNextColumn(); ImGui::TextColored(kRed, "%s", commafy(s.ask).c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%s", commafy(s.mid).c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%s", commafy(s.ask - s.bid).c_str());
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
                    ImGui::TableNextColumn(); ImGui::Text("%s", commafy(s.avg_price).c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(pnl_color(s.unrealized), "%s", signed_money(s.unrealized).c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(pnl_color(s.realized), "%s", signed_money(s.realized).c_str());
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

            // --- Ticker Search (browse/search the full product catalog) ---------
            ImGui::Begin("Ticker Search");
            static char filter[32] = "";
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "search ticker (e.g. SOL)...", filter, sizeof(filter));
            for (char* p = filter; *p; ++p) *p = (char)toupper(*p);
            ImGui::TextDisabled("%zu products", v.products.size());
            ImGui::BeginChild("catalog", ImVec2(0, 0), false);
            int shown = 0;
            for (const auto& id : v.products) {
                if (filter[0] && id.find(filter) == std::string::npos) continue;
                if (++shown > 500) break;  // keep the list responsive
                if (ImGui::Selectable(id.c_str()))
                    engine->post({Command::AddSymbol, id, 0, false});
            }
            ImGui::EndChild();
            ImGui::End();

            // --- Price Chart (focused symbol) -----------------------------------
            ImGui::Begin("Price Chart");
            ImGui::TextColored(kAmber, "%s", v.focus.empty() ? "(select a symbol)" : v.focus.c_str());
            if (v.price_history.size() >= 2) {
                if (ImPlot::BeginPlot("##px", ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                    // Fit X to the data, but give Y ~15% headroom top and bottom so
                    // the line never hugs the window edges.
                    double lo = *std::min_element(v.price_history.begin(), v.price_history.end());
                    double hi = *std::max_element(v.price_history.begin(), v.price_history.end());
                    double pad = (hi - lo) * 0.15;
                    if (pad <= 0) pad = std::abs(hi) * 0.001 + 1e-6;
                    ImPlot::SetupAxes("ticks", "price", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, lo - pad, hi + pad, ImPlotCond_Always);
                    ImPlotSpec spec;
                    spec.LineColor = kAmber;
                    spec.LineWeight = 1.6f;
                    ImPlot::PlotLine(v.focus.c_str(), v.price_history.data(),
                                     (int)v.price_history.size(), 1.0, 0.0, spec);
                    ImPlot::EndPlot();
                }
            } else {
                ImGui::TextDisabled("waiting for trades...");
            }
            ImGui::End();

            // --- PnL chart ------------------------------------------------------
            ImGui::Begin("PnL");
            if (v.pnl_history.size() >= 2) {
                if (ImPlot::BeginPlot("##pnl", ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                    const int n = (int)v.pnl_history.size();
                    // Symmetric Y limits so the zero line stays in the middle (the
                    // direction of profit/loss is always visible) with headroom on top.
                    double m = 0.0;
                    for (double y : v.pnl_history) m = std::max(m, std::abs(y));
                    double top = m * 1.25;
                    if (top < 1.0) top = 1.0;
                    ImPlot::SetupAxes("t", "$", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -top, top, ImPlotCond_Always);

                    // zero reference line
                    double zx[2] = {0.0, (double)(n - 1)};
                    double zy[2] = {0.0, 0.0};
                    ImPlotSpec zs; zs.LineColor = kAmberDim; zs.LineWeight = 1.0f;
                    ImPlot::PlotLine("##zero", zx, zy, 2, zs);

                    // Split the series into profit (green) / loss (red) via NaN masking.
                    static std::vector<double> up, down;
                    up.assign(n, NAN);
                    down.assign(n, NAN);
                    for (int i = 0; i < n; ++i) {
                        double y = v.pnl_history[i];
                        if (y >= 0) up[i] = y;
                        if (y <= 0) down[i] = y;  // boundary in both so segments join at zero
                    }
                    ImPlotSpec gs; gs.LineColor = kGreen; gs.LineWeight = 1.6f;
                    ImPlotSpec rs; rs.LineColor = kRed;   rs.LineWeight = 1.6f;
                    ImPlot::PlotLine("profit", up.data(), n, 1.0, 0.0, gs);
                    ImPlot::PlotLine("loss", down.data(), n, 1.0, 0.0, rs);
                    ImPlot::EndPlot();
                }
                ImGui::TextColored(pnl_color(v.total_pnl), "  now: %s", signed_money(v.total_pnl).c_str());
            } else {
                ImGui::TextDisabled("accumulating...");
            }
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
