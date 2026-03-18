// ADW AI Mission Control Screen
// ─────────────────────────────────────────────────────────────────────────────
// Unified command center: ML signal confidence, bot health, risk metrics,
// mode controls for the ADW AI Ltd autonomous trading stack.
// ─────────────────────────────────────────────────────────────────────────────

#include "adw_mission_control_screen.h"
#include "ui/yoga_helpers.h"
#include "python/python_runner.h"
#include "ui/theme.h"
#include "core/logger.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace fincept::adw {

using json = nlohmann::json;

// ─── helpers ─────────────────────────────────────────────────────────────────

static std::string safe_str(const json& j, const std::string& key,
                             const std::string& def = "") {
    if (j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return def;
}

static float safe_float(const json& j, const std::string& key, float def = 0.0f) {
    if (j.contains(key)) {
        if (j[key].is_number()) return j[key].get<float>();
        if (j[key].is_null())   return def;
    }
    return def;
}

static bool safe_bool(const json& j, const std::string& key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

// ─── Static color helpers ─────────────────────────────────────────────────────

ImVec4 ADWMissionControlScreen::conf_color(float c) {
    if (c >= 80.0f) return theme::colors::MARKET_GREEN;
    if (c >= 65.0f) return theme::colors::WARNING;
    return theme::colors::MARKET_RED;
}

ImVec4 ADWMissionControlScreen::dir_color(const std::string& dir) {
    return (dir == "LONG") ? theme::colors::MARKET_GREEN : theme::colors::MARKET_RED;
}

const char* ADWMissionControlScreen::mode_icon(const std::string& mode) {
    if (mode == "AGGRESSIVE") return "⚡";
    if (mode == "CONSERVATIVE") return "🛡";
    if (mode == "LIVE") return "🔴";
    return "📄"; // PAPER
}

// ─── init / refresh lifecycle ─────────────────────────────────────────────────

void ADWMissionControlScreen::init() {
    initialized_ = true;
    trigger_refresh();
}

void ADWMissionControlScreen::trigger_refresh() {
    if (loading_.exchange(true)) return;  // already in flight
    last_raw_json_.clear();
    fetch_future_ = python::execute_async("adw_mission_control", {"all"});
}

void ADWMissionControlScreen::poll_fetch() {
    if (!loading_) return;
    if (!fetch_future_.valid()) { loading_ = false; return; }

    auto status = fetch_future_.wait_for(std::chrono::milliseconds(0));
    if (status != std::future_status::ready) return;

    auto result = fetch_future_.get();
    loading_ = false;
    last_refresh_ = std::chrono::steady_clock::now();

    if (result.success) {
        last_raw_json_ = result.output;
        parse_payload(result.output);
    } else {
        std::lock_guard<std::mutex> lock(payload_mutex_);
        payload_.ok = false;
        payload_.error = result.error.empty() ? "Python script failed" : result.error;
    }
}

void ADWMissionControlScreen::parse_payload(const std::string& raw) {
    std::lock_guard<std::mutex> lock(payload_mutex_);
    try {
        auto j = json::parse(raw, nullptr, false);
        if (j.is_discarded()) { payload_.error = "JSON parse error"; return; }

        payload_.ok           = safe_bool(j, "ok");
        payload_.error        = safe_str(j, "error");
        payload_.generated_at = safe_str(j, "generated_at");
        payload_.combaus_root = safe_str(j, "combaus_root");

        auto parse_bot = [](const json& b) -> BotStatus {
            BotStatus s;
            s.mode           = safe_str(b, "mode", "PAPER");
            s.models_ready   = safe_bool(b, "models_ready");
            s.avg_confidence = safe_float(b, "avg_confidence");

            if (b.contains("circuit_breaker") && b["circuit_breaker"].is_object()) {
                const auto& cb = b["circuit_breaker"];
                s.circuit_breaker.triggered = safe_bool(cb, "triggered");
                s.circuit_breaker.reason    = safe_str(cb, "reason");
            }
            if (b.contains("equity") && b["equity"].is_object()) {
                const auto& eq = b["equity"];
                s.equity.equity    = safe_float(eq, "equity");
                s.equity.daily_pnl = safe_float(eq, "daily_pnl");
                s.equity.drawdown  = safe_float(eq, "drawdown");
            }
            if (b.contains("signals") && b["signals"].is_array()) {
                for (const auto& sig : b["signals"]) {
                    SignalEntry e;
                    e.symbol       = safe_str(sig, "symbol");
                    e.direction    = safe_str(sig, "direction", "LONG");
                    e.confidence   = safe_float(sig, "confidence");
                    e.price        = safe_float(sig, "price");
                    e.rsi          = safe_float(sig, "rsi");
                    e.model_loaded = safe_bool(sig, "model_loaded");
                    e.timestamp    = sig.value("timestamp", int64_t{0});
                    s.signals.push_back(e);
                }
            }
            return s;
        };

        if (j.contains("futures") && j["futures"].is_object())
            payload_.futures = parse_bot(j["futures"]);
        if (j.contains("forex") && j["forex"].is_object())
            payload_.forex = parse_bot(j["forex"]);

    } catch (const std::exception& e) {
        payload_.ok    = false;
        payload_.error = std::string("Parse exception: ") + e.what();
    }
}

// ─── top-level render ─────────────────────────────────────────────────────────

void ADWMissionControlScreen::render() {
    using namespace theme::colors;

    if (!initialized_) init();
    poll_fetch();

    // Auto-refresh
    if (auto_refresh_ && !loading_) {
        auto elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - last_refresh_).count();
        if (elapsed > AUTO_REFRESH_SEC) trigger_refresh();
    }

    ui::ScreenFrame frame("##adw_mc", ImVec2(0,0), ImVec4(0.04f, 0.04f, 0.06f, 1.0f));
    if (!frame.begin()) { frame.end(); return; }

    const float w = frame.width();
    const float h = frame.height();

    // Layout: header(50) | subnav(36) | content(flex) | status(26)
    auto layout = ui::vstack_layout(w, h, {50.0f, 36.0f, -1.0f, 26.0f});

    render_header(w);
    render_top_nav(w);

    float content_h = layout.heights[2];
    ImGui::BeginChild("##adw_content", ImVec2(w, content_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        MissionPayload snap;
        { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

        switch (active_view_) {
            case SubView::Overview: render_overview(w, content_h); break;
            case SubView::Futures:  render_futures(w, content_h);  break;
            case SubView::Forex:    render_forex(w, content_h);    break;
            case SubView::Risk:     render_risk(w, content_h);     break;
            case SubView::Signals:  render_signals(w, content_h);  break;
        }
    }
    ImGui::EndChild();

    render_status_bar(w, layout.heights[3]);

    frame.end();
}

// ─── header ───────────────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_header(float w) {
    using namespace theme::colors;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.04f, 0.08f, 1.0f));
    ImGui::BeginChild("##adw_hdr", ImVec2(w, 50.0f), false);

    ImGui::SetCursorPos(ImVec2(12.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
    ImGui::TextUnformatted("⚡ ADW AI MISSION CONTROL");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::SetCursorPosY(8.0f);
    ImGui::TextColored(TEXT_DIM, "  |  ADW AI Ltd  |  Autonomous Trading Command Center");

    // Right-side: refresh button + auto toggle
    float btn_x = w - 220.0f;
    ImGui::SetCursorPos(ImVec2(btn_x, 12.0f));

    if (loading_) {
        ImGui::TextColored(WARNING, "⟳ Fetching...");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.05f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.10f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ACCENT);
        if (ImGui::Button("⟳ REFRESH", ImVec2(90, 24))) trigger_refresh();
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine(0, 8);
    ImGui::SetCursorPosY(14.0f);
    ImGui::Checkbox("Auto", &auto_refresh_);

    ImGui::SameLine(0, 12);
    ImGui::SetCursorPosY(14.0f);
    ImGui::Checkbox("Raw JSON", &show_raw_json_);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── sub-view nav ─────────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_top_nav(float w) {
    using namespace theme::colors;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.10f, 1.0f));
    ImGui::BeginChild("##adw_nav", ImVec2(w, 36.0f), false);
    ImGui::SetCursorPos(ImVec2(10.0f, 5.0f));

    struct NavItem { const char* label; SubView view; };
    static constexpr NavItem items[] = {
        {"Overview",  SubView::Overview},
        {"Futures",   SubView::Futures},
        {"Forex",     SubView::Forex},
        {"Risk",      SubView::Risk},
        {"Signals",   SubView::Signals},
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(4, 0));

    for (const auto& item : items) {
        bool active = (active_view_ == item.view);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ACCENT_BG);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ACCENT_BG);
            ImGui::PushStyleColor(ImGuiCol_Text,          ACCENT);
            ImGui::PushStyleColor(ImGuiCol_Border,        ACCENT);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BG_HOVER);
            ImGui::PushStyleColor(ImGuiCol_Text,          TEXT_SECONDARY);
            ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0,0,0,0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        }
        if (ImGui::Button(item.label)) active_view_ = item.view;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
    }

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── confidence bar widget ────────────────────────────────────────────────────

void ADWMissionControlScreen::render_confidence_bar(float confidence, float width, const char* id) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float bar_h = 8.0f;
    float filled = width * std::clamp(confidence / 100.0f, 0.0f, 1.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + bar_h),
                      IM_COL32(40, 40, 50, 255), 2.0f);
    // Fill
    ImVec4 fc = conf_color(confidence);
    dl->AddRectFilled(pos, ImVec2(pos.x + filled, pos.y + bar_h),
                      ImGui::ColorConvertFloat4ToU32(fc), 2.0f);
    // Dummy to advance cursor
    ImGui::InvisibleButton(id, ImVec2(width, bar_h));
}

// ─── circuit breaker badge ────────────────────────────────────────────────────

void ADWMissionControlScreen::render_circuit_breaker_badge(const CircuitBreaker& cb) {
    using namespace theme::colors;
    if (cb.triggered) {
        ImGui::TextColored(MARKET_RED, "⛔ CIRCUIT BREAKER TRIGGERED");
        if (!cb.reason.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(TEXT_DIM, "(%s)", cb.reason.c_str());
        }
    } else {
        ImGui::TextColored(MARKET_GREEN, "✓ Circuit Breaker OK");
    }
}

// ─── bot summary card ─────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_bot_card(const char* title, const BotStatus& bot,
                                               ImVec4 accent, float card_w, float card_h) {
    using namespace theme::colors;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(accent.x, accent.y, accent.z, 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   2.0f);

    char child_id[64];
    std::snprintf(child_id, sizeof(child_id), "##card_%s", title);
    ImGui::BeginChild(child_id, ImVec2(card_w, card_h), true);

    // Title row
    ImGui::TextColored(accent, "%s %s", mode_icon(bot.mode), title);
    ImGui::SameLine(card_w - 80.0f);
    ImGui::TextColored(bot.models_ready ? MARKET_GREEN : WARNING,
                       bot.models_ready ? "●  MODEL" : "◌  DEMO");

    ImGui::Separator();

    // Mode
    ImGui::TextColored(TEXT_DIM, "Mode  ");
    ImGui::SameLine();
    ImGui::TextColored(bot.mode == "AGGRESSIVE" ? MARKET_RED : MARKET_GREEN,
                       "%s", bot.mode.c_str());

    // Avg confidence meter
    ImGui::Spacing();
    ImGui::TextColored(TEXT_DIM, "Avg Confidence");
    char conf_id[64];
    std::snprintf(conf_id, sizeof(conf_id), "##conf_%s", title);
    render_confidence_bar(bot.avg_confidence, card_w - 20.0f, conf_id);
    ImGui::SameLine(0, 6);
    ImGui::TextColored(conf_color(bot.avg_confidence), "%.0f%%", bot.avg_confidence);

    // Equity (if available)
    if (bot.equity.equity > 0.0f) {
        ImGui::Spacing();
        char eq_buf[32];
        std::snprintf(eq_buf, sizeof(eq_buf), "$%.0f", bot.equity.equity);
        ImGui::TextColored(TEXT_SECONDARY, "Equity ");
        ImGui::SameLine();
        ImGui::TextColored(TEXT_PRIMARY, "%s", eq_buf);
        ImGui::SameLine(0, 12);
        ImGui::TextColored(TEXT_DIM, "Daily P&L ");
        ImGui::SameLine();
        float dpnl = bot.equity.daily_pnl;
        ImGui::TextColored(dpnl >= 0 ? MARKET_GREEN : MARKET_RED,
                           "%+.2f", dpnl);
        if (bot.equity.drawdown > 0.0f) {
            ImGui::TextColored(TEXT_DIM, "Drawdown ");
            ImGui::SameLine();
            ImGui::TextColored(MARKET_RED, "%.1f%%", bot.equity.drawdown);
        }
    } else {
        ImGui::Spacing();
        ImGui::TextColored(TEXT_DISABLED, "No equity data (start paper loop)");
    }

    // Circuit breaker
    ImGui::Spacing();
    render_circuit_breaker_badge(bot.circuit_breaker);

    // Signal count
    ImGui::Spacing();
    ImGui::TextColored(TEXT_DIM, "%d instruments monitored", (int)bot.signals.size());

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── OVERVIEW ─────────────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_overview(float w, float h) {
    using namespace theme::colors;

    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    if (!snap.ok && !snap.error.empty()) {
        ImGui::SetCursorPosY(h * 0.4f);
        float tw = ImGui::CalcTextSize(snap.error.c_str()).x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(MARKET_RED, "Error: %s", snap.error.c_str());
        return;
    }

    if (loading_ && snap.generated_at.empty()) {
        ImGui::SetCursorPosY(h * 0.45f);
        const char* msg = "⟳  Loading signals...";
        float tw = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(ACCENT, "%s", msg);
        return;
    }

    float pad = 12.0f;
    float gap = 10.0f;
    float card_w = (w - pad * 2 - gap) / 2.0f;
    float card_h = 190.0f;

    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::BeginGroup();
    render_bot_card("FUTURES BOT",  snap.futures, ImVec4(0.3f, 0.7f, 1.0f, 1.0f),  card_w, card_h);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    render_bot_card("FOREX BOT",    snap.forex,   ImVec4(0.2f, 0.9f, 0.5f, 1.0f),  card_w, card_h);
    ImGui::EndGroup();

    // ── Top futures signals preview ──────────────────────────────────────────
    float signals_y = pad + card_h + gap;
    ImGui::SetCursorPos(ImVec2(pad, signals_y));
    ImGui::TextColored(ACCENT, "TOP SIGNALS");
    ImGui::Separator();

    float col_w = (w - pad * 2) / 2.0f;
    float sig_child_h = h - signals_y - 40.0f;

    ImGui::BeginChild("##ov_futures_sig", ImVec2(col_w - gap/2, sig_child_h), false);
    ImGui::TextColored(ImVec4(0.3f,0.7f,1.0f,1.0f), "Futures");
    ImGui::Separator();
    for (const auto& s : snap.futures.signals) {
        ImGui::TextColored(TEXT_PRIMARY, "%-10s", s.symbol.c_str());
        ImGui::SameLine(90);
        ImGui::TextColored(dir_color(s.direction), "%-6s", s.direction.c_str());
        ImGui::SameLine(140);
        char bid[32]; std::snprintf(bid, sizeof(bid), "##fb_%s", s.symbol.c_str());
        render_confidence_bar(s.confidence, 120.0f, bid);
        ImGui::SameLine(0, 6);
        ImGui::TextColored(conf_color(s.confidence), "%.0f%%", s.confidence);
    }
    ImGui::EndChild();

    ImGui::SameLine(0, gap);
    ImGui::BeginChild("##ov_forex_sig", ImVec2(col_w - gap/2, sig_child_h), false);
    ImGui::TextColored(ImVec4(0.2f,0.9f,0.5f,1.0f), "Forex");
    ImGui::Separator();
    for (const auto& s : snap.forex.signals) {
        std::string sym = s.symbol; // strip =X suffix for display
        if (sym.size() > 2 && sym.substr(sym.size()-2) == "=X")
            sym = sym.substr(0, sym.size()-2);
        ImGui::TextColored(TEXT_PRIMARY, "%-8s", sym.c_str());
        ImGui::SameLine(80);
        ImGui::TextColored(dir_color(s.direction), "%-6s", s.direction.c_str());
        ImGui::SameLine(130);
        char bid[32]; std::snprintf(bid, sizeof(bid), "##fx_%s", s.symbol.c_str());
        render_confidence_bar(s.confidence, 120.0f, bid);
        ImGui::SameLine(0, 6);
        ImGui::TextColored(conf_color(s.confidence), "%.0f%%", s.confidence);
    }
    ImGui::EndChild();

    // Raw JSON panel (optional)
    if (show_raw_json_ && !last_raw_json_.empty()) {
        ImGui::SetCursorPosX(pad);
        ImGui::Separator();
        ImGui::TextColored(TEXT_DIM, "Raw JSON:");
        ImGui::BeginChild("##raw_json", ImVec2(w - pad*2, 120.0f), true,
                           ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(last_raw_json_.c_str());
        ImGui::EndChild();
    }
}

// ─── FUTURES detail view ──────────────────────────────────────────────────────

void ADWMissionControlScreen::render_futures(float w, float h) {
    using namespace theme::colors;
    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    float pad = 12.0f;
    ImGui::SetCursorPos(ImVec2(pad, pad));

    // Header info row
    ImGui::TextColored(ImVec4(0.3f,0.7f,1.0f,1.0f), "⬛ FUTURES BOT");
    ImGui::SameLine(0, 12);
    ImGui::TextColored(TEXT_DIM, "Mode:");
    ImGui::SameLine();
    ImGui::TextColored(TEXT_PRIMARY, "%s", snap.futures.mode.c_str());
    ImGui::SameLine(0, 16);
    render_circuit_breaker_badge(snap.futures.circuit_breaker);

    ImGui::Separator();
    render_signal_table(snap.futures.signals, w - pad*2, h - 60.0f);
}

// ─── FOREX detail view ────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_forex(float w, float h) {
    using namespace theme::colors;
    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    float pad = 12.0f;
    ImGui::SetCursorPos(ImVec2(pad, pad));

    ImGui::TextColored(ImVec4(0.2f,0.9f,0.5f,1.0f), "💱 FOREX BOT");
    ImGui::SameLine(0, 12);
    ImGui::TextColored(TEXT_DIM, "Mode:");
    ImGui::SameLine();
    ImGui::TextColored(snap.forex.mode == "AGGRESSIVE" ? MARKET_RED : MARKET_GREEN,
                       "%s %s", mode_icon(snap.forex.mode), snap.forex.mode.c_str());
    ImGui::SameLine(0, 16);
    render_circuit_breaker_badge(snap.forex.circuit_breaker);

    ImGui::Separator();
    render_signal_table(snap.forex.signals, w - pad*2, h - 60.0f);
}

// ─── Signal table (shared) ────────────────────────────────────────────────────

void ADWMissionControlScreen::render_signal_table(const std::vector<SignalEntry>& signals,
                                                   float w, float h) {
    using namespace theme::colors;

    ImGui::BeginChild("##sig_table_scroll", ImVec2(w, h), false);

    if (signals.empty()) {
        ImGui::SetCursorPosY(h * 0.4f);
        ImGui::TextColored(TEXT_DISABLED, "No signal data — trigger a refresh");
        ImGui::EndChild();
        return;
    }

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
    ImGui::TextUnformatted("Symbol");      ImGui::SameLine(110);
    ImGui::TextUnformatted("Direction");   ImGui::SameLine(195);
    ImGui::TextUnformatted("Confidence");  ImGui::SameLine(380);
    ImGui::TextUnformatted("RSI");         ImGui::SameLine(430);
    ImGui::TextUnformatted("Price");       ImGui::SameLine(510);
    ImGui::TextUnformatted("Model");
    ImGui::PopStyleColor();
    ImGui::Separator();

    for (size_t i = 0; i < signals.size(); i++) {
        const auto& s = signals[i];

        ImGui::PushID(static_cast<int>(i));

        // Alternate row bg
        if (i % 2 == 0) {
            ImVec2 rp  = ImGui::GetCursorScreenPos();
            ImVec2 re  = ImVec2(rp.x + w, rp.y + ImGui::GetTextLineHeightWithSpacing());
            ImGui::GetWindowDrawList()->AddRectFilled(
                rp, re, IM_COL32(30, 30, 40, 180));
        }

        // Symbol
        std::string sym = s.symbol;
        if (sym.size() > 2 && sym.substr(sym.size()-2) == "=X")
            sym = sym.substr(0, sym.size()-2);
        ImGui::TextColored(TEXT_PRIMARY, "%-10s", sym.c_str());

        // Direction
        ImGui::SameLine(110);
        ImGui::TextColored(dir_color(s.direction), "%-8s", s.direction.c_str());

        // Confidence bar
        ImGui::SameLine(195);
        render_confidence_bar(s.confidence, 160.0f, "##cbar");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(conf_color(s.confidence), "%.1f%%", s.confidence);

        // RSI
        ImGui::SameLine(430);
        ImVec4 rsi_col = (s.rsi > 70) ? MARKET_RED : (s.rsi < 30) ? MARKET_GREEN : TEXT_SECONDARY;
        ImGui::TextColored(rsi_col, "%.0f", s.rsi);

        // Price
        ImGui::SameLine(510);
        if (s.price > 0.0f) {
            ImGui::TextColored(TEXT_SECONDARY, "%.4f", s.price);
        } else {
            ImGui::TextColored(TEXT_DISABLED, "--");
        }

        // Model status
        ImGui::SameLine(600);
        if (s.model_loaded) {
            ImGui::TextColored(MARKET_GREEN, "✓ live");
        } else {
            ImGui::TextColored(TEXT_DISABLED, "demo");
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ─── RISK view ────────────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_risk(float w, float h) {
    using namespace theme::colors;
    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    float pad = 12.0f;
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::TextColored(ACCENT, "⚠  RISK DASHBOARD");
    ImGui::Separator();

    float col_w = (w - pad * 2 - 12.0f) / 2.0f;

    // ── Futures risk ─────────────────────────────────────────────────────────
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.3f,0.7f,1.0f,1.0f), "Futures Risk");
    ImGui::Separator();
    render_circuit_breaker_badge(snap.futures.circuit_breaker);
    ImGui::Spacing();
    if (snap.futures.equity.equity > 0) {
        ImGui::TextColored(TEXT_DIM, "Equity:    "); ImGui::SameLine();
        ImGui::TextColored(TEXT_PRIMARY, "$%.2f", snap.futures.equity.equity);
        ImGui::TextColored(TEXT_DIM, "Daily P&L: "); ImGui::SameLine();
        float dpnl = snap.futures.equity.daily_pnl;
        ImGui::TextColored(dpnl >= 0 ? MARKET_GREEN : MARKET_RED, "%+.2f", dpnl);
        ImGui::TextColored(TEXT_DIM, "Drawdown:  "); ImGui::SameLine();
        ImGui::TextColored(MARKET_RED, "%.1f%%", snap.futures.equity.drawdown);
    } else {
        ImGui::TextColored(TEXT_DISABLED, "No equity data");
        ImGui::TextColored(TEXT_DISABLED, "Run: python main.py paper");
    }
    ImGui::Spacing();
    ImGui::TextColored(TEXT_DIM, "Avg Confidence:");
    render_confidence_bar(snap.futures.avg_confidence, col_w - 20, "##frisk_conf");
    ImGui::SameLine(0,4);
    ImGui::TextColored(conf_color(snap.futures.avg_confidence),
                       "%.1f%%", snap.futures.avg_confidence);
    ImGui::EndGroup();

    ImGui::SameLine(col_w + 12.0f);

    // ── Forex risk ───────────────────────────────────────────────────────────
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.2f,0.9f,0.5f,1.0f), "Forex Risk");
    ImGui::Separator();
    render_circuit_breaker_badge(snap.forex.circuit_breaker);
    ImGui::Spacing();
    if (snap.forex.equity.equity > 0) {
        ImGui::TextColored(TEXT_DIM, "Equity:    "); ImGui::SameLine();
        ImGui::TextColored(TEXT_PRIMARY, "$%.2f", snap.forex.equity.equity);
        ImGui::TextColored(TEXT_DIM, "Daily P&L: "); ImGui::SameLine();
        float dpnl = snap.forex.equity.daily_pnl;
        ImGui::TextColored(dpnl >= 0 ? MARKET_GREEN : MARKET_RED, "%+.2f", dpnl);
        ImGui::TextColored(TEXT_DIM, "Drawdown:  "); ImGui::SameLine();
        ImGui::TextColored(MARKET_RED, "%.1f%%", snap.forex.equity.drawdown);
    } else {
        ImGui::TextColored(TEXT_DISABLED, "No equity data");
        ImGui::TextColored(TEXT_DISABLED, "Run: python main.py paper");
    }
    ImGui::Spacing();
    ImGui::TextColored(TEXT_DIM, "Mode:   "); ImGui::SameLine();
    ImGui::TextColored(snap.forex.mode == "AGGRESSIVE" ? MARKET_RED : MARKET_GREEN,
                       "%s %s", mode_icon(snap.forex.mode), snap.forex.mode.c_str());
    ImGui::Spacing();
    ImGui::TextColored(TEXT_DIM, "Avg Confidence:");
    render_confidence_bar(snap.forex.avg_confidence, col_w - 20, "##fxrisk_conf");
    ImGui::SameLine(0,4);
    ImGui::TextColored(conf_color(snap.forex.avg_confidence),
                       "%.1f%%", snap.forex.avg_confidence);
    ImGui::EndGroup();

    // ── Mode switcher ─────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ACCENT, "FOREX MODE SWITCHER");
    ImGui::Spacing();

    ImGui::TextColored(TEXT_DIM, "Set FOREX_MODE env var and restart forex-bot:");
    ImGui::Spacing();

    auto mode_btn = [&](const char* label, const char* env_val, ImVec4 color) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(color.x*0.2f, color.y*0.2f, color.z*0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x*0.4f, color.y*0.4f, color.z*0.4f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          color);
        if (ImGui::Button(label, ImVec2(140, 30))) {
            // Emit a notification — actual mode change requires restarting the bot
            LOG_INFO("ADW", "User requested forex mode: %s", env_val);
            // In future: could write a mode file the bot polls
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Set FOREX_MODE=%s and restart", env_val);
    };

    mode_btn("🛡 CONSERVATIVE", "CONSERVATIVE", MARKET_GREEN);
    ImGui::SameLine(0, 10);
    mode_btn("⚡ AGGRESSIVE",   "AGGRESSIVE",   MARKET_RED);
    ImGui::SameLine(0, 10);
    mode_btn("📄 PAPER",        "PAPER",         WARNING);
}

// ─── SIGNALS all-up view ──────────────────────────────────────────────────────

void ADWMissionControlScreen::render_signals(float w, float h) {
    using namespace theme::colors;
    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    float pad = 12.0f;
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::TextColored(ACCENT, "ALL SIGNALS");

    char ts_buf[64] = "never";
    if (!snap.generated_at.empty())
        std::snprintf(ts_buf, sizeof(ts_buf), "%s", snap.generated_at.c_str());
    ImGui::SameLine();
    ImGui::TextColored(TEXT_DIM, "   Last update: %s", ts_buf);

    ImGui::Separator();

    // Combined table: futures first, then forex
    std::vector<SignalEntry> all;
    all.insert(all.end(), snap.futures.signals.begin(), snap.futures.signals.end());
    all.insert(all.end(), snap.forex.signals.begin(),   snap.forex.signals.end());

    render_signal_table(all, w - pad*2, h - 60.0f);
}

// ─── status bar ───────────────────────────────────────────────────────────────

void ADWMissionControlScreen::render_status_bar(float w, float h) {
    using namespace theme::colors;

    MissionPayload snap;
    { std::lock_guard<std::mutex> lk(payload_mutex_); snap = payload_; }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
    ImGui::BeginChild("##adw_status", ImVec2(w, h), false);
    ImGui::SetCursorPosY(5.0f);

    if (loading_) {
        ImGui::TextColored(WARNING, "  ⟳ Fetching signal data...");
    } else if (!snap.ok && !snap.error.empty()) {
        ImGui::TextColored(MARKET_RED, "  ✗ %s", snap.error.c_str());
    } else {
        ImGui::TextColored(TEXT_DIM, "  Futures: %zu signals  |  Forex: %zu signals",
                           snap.futures.signals.size(), snap.forex.signals.size());
    }

    ImGui::SameLine(w - 280.0f);
    ImGui::TextColored(TEXT_DISABLED, "ADW AI Ltd  |  Mission Control v1.0");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace fincept::adw
