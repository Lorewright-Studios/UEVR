#pragma once

// Meta PC runtime ASW (Asynchronous SpaceWarp) control.
//
// On Meta's PC runtime (Quest over Link or Air Link, Rift), a steady half refresh rate with ASW is far smoother
// than an uneven 40-60 fps, but the runtime's own "Auto" mode often does not engage for UEVR games.
// Meta gives apps no API for the ASW mode. This controller therefore uses, in order:
//   1. Meta's own debug CLI (OculusDebugToolCLI.exe: "server:asw.Sim45" / "server:asw.Auto"), when it runs
//      without elevation;
//   2. the runtime's keyboard shortcuts (Ctrl+Numpad3 = force ASW, Ctrl+Numpad4 = automatic).
// Every change is checked against the frame cadence the runtime reports through xrWaitFrame.
// It never acts on any other runtime (SteamVR Motion Smoothing and Virtual Desktop SSW are left alone).
//
// Modes (VR_MetaASWMode):
//   Smart        - force ASW only after the game has run clearly below the refresh rate for a sustained time
//                  during play; decided at most once per session.
//   Leave alone  - never touch the runtime's setting.
//   Always force - force ASW shortly after the session starts.
// When this controller forced ASW, it sets it back to automatic when the session stops, the game window is
// destroyed, the mode is changed to "Leave alone", or the process exits.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace uevr::meta_asw {

enum class Mode : int32_t {
    Smart = 0,
    LeaveAlone = 1,
    AlwaysForce = 2,
};

class Controller {
public:
    static Controller& get();

    void set_runtime_name(std::string_view name);
    bool is_meta_runtime() const { return m_is_meta.load(std::memory_order_acquire); }

    // Call after every successful xrWaitFrame. Cheap; any switching runs on a worker thread.
    void on_wait_frame(int64_t predicted_display_time, int64_t predicted_display_period, bool focused, Mode mode);

    // Restore points. Each only acts if this controller forced ASW.
    void on_session_stopping();
    void on_window_destroyed();
    void on_process_exit(); // from DllMain: keyboard shortcut only, never spawns a process

    const char* state_name() const;

private:
    Controller() = default;

    enum class State : int32_t {
        Idle,       // watching (or nothing to do)
        Applying,   // worker is switching
        Verifying,  // waiting to see the half-rate cadence
        Forced,     // ASW forced by us
        Failed,     // could not force; no further attempts this session
        Restored,   // set back to automatic
    };

    enum class Method : int32_t {
        None,
        Cli,
        Hotkey,
    };

    void start_force(Method method);
    void start_restore(bool wait);
    void reset_window();

    std::atomic<bool> m_is_meta{false};
    std::atomic<State> m_state{State::Idle};
    std::atomic<Method> m_method{Method::None};
    std::atomic<bool> m_worker_busy{false};
    std::atomic<bool> m_cli_unavailable{false};

    std::mutex m_mtx{};
    std::string m_runtime_name{};

    int64_t m_last_display_time{0};
    int64_t m_min_period{0};
    std::chrono::steady_clock::time_point m_focused_since{};
    bool m_was_focused{false};

    // one-second cadence windows
    int64_t m_window_sum{0};
    uint32_t m_window_frames{0};
    uint32_t m_window_half_rate{0};
    uint32_t m_slow_streak{0};
    uint32_t m_locked_windows{0};
    uint32_t m_verify_windows{0};
    uint32_t m_unlocked_windows{0};
    std::chrono::steady_clock::time_point m_applied_at{};
};

} // namespace uevr::meta_asw
