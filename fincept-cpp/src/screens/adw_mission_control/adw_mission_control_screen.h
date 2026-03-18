#pragma once
// ADW AI Mission Control Screen
// ─────────────────────────────────────────────────────────────────────────────
// A unified command-center that surfaces real-time ML signal confidence,
// bot health, risk metrics, and one-click mode controls for the ADW AI Ltd
// autonomous trading stack (futures-bot + forex-bot).
//
// Data is fetched from the embedded Python bridge:
//   scripts/adw_mission_control.py  →  JSON payload
//
// Sub-views: Overview | Futures | Forex | Risk | Signals
// ─────────────────────────────────────────────────────────────────────────────

#include <imgui.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <future>
#include <chrono>

namespace fincept::adw {

// ── Data structures mirroring Python JSON output ─────────────────────────────

struct SignalEntry {
    std::string symbol;
    std::string direction;   // "LONG" | "SHORT"
    float       confidence;  // 0–100
    float       price;       // 0 = unavailable
    float       rsi;
    bool        model_loaded;
    int64_t     timestamp;
};

struct CircuitBreaker {
    bool        triggered = false;
    std::string reason;
};

struct EquitySnapshot {
    float equity    = 0.0f;   // 0 = unavailable
    float daily_pnl = 0.0f;
    float drawdown  = 0.0f;
};

struct BotStatus {
    std::string    mode;             // "PAPER" | "LIVE" | "CONSERVATIVE" | "AGGRESSIVE"
    bool           models_ready = false;
    float          avg_confidence = 0.0f;
    CircuitBreaker circuit_breaker;
    EquitySnapshot equity;
    std::vector<SignalEntry> signals;
};

struct MissionPayload {
    bool        ok = false;
    std::string error;
    std::string generated_at;
    std::string combaus_root;
    BotStatus   futures;
    BotStatus   forex;
};

// ── Screen class ──────────────────────────────────────────────────────────────

class ADWMissionControlScreen {
public:
    void render();

private:
    bool initialized_ = false;
    void init();

    // Sub-view navigation
    enum class SubView { Overview, Futures, Forex, Risk, Signals };
    SubView active_view_ = SubView::Overview;

    // Data refresh
    MissionPayload            payload_;
    std::mutex                payload_mutex_;
    std::atomic<bool>         loading_{false};
    std::future<std::string>  fetch_future_;
    std::chrono::steady_clock::time_point last_refresh_{};
    static constexpr float AUTO_REFRESH_SEC = 60.0f;  // refresh every 60 s
    std::string               last_raw_json_;

    // UI state
    bool show_raw_json_    = false;
    bool auto_refresh_     = true;

    // Data loading
    void trigger_refresh();
    void poll_fetch();
    void parse_payload(const std::string& json);

    // Sub-view renderers
    void render_header(float w);
    void render_top_nav(float w);
    void render_status_bar(float w, float h);
    void render_overview(float w, float h);
    void render_futures(float w, float h);
    void render_forex(float w, float h);
    void render_risk(float w, float h);
    void render_signals(float w, float h);

    // Shared widgets
    void render_signal_table(const std::vector<SignalEntry>& signals, float w, float h);
    void render_bot_card(const char* title, const BotStatus& bot,
                         ImVec4 accent, float card_w, float card_h);
    void render_confidence_bar(float confidence, float width, const char* id);
    void render_circuit_breaker_badge(const CircuitBreaker& cb);
    static ImVec4 conf_color(float c);
    static ImVec4 dir_color(const std::string& dir);
    static const char* mode_icon(const std::string& mode);
};

} // namespace fincept::adw
