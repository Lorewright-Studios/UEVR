#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "MetaAsw.hpp"

namespace uevr::meta_asw {
namespace {
using Clock = std::chrono::steady_clock;

constexpr auto SMART_WARMUP = std::chrono::seconds{20};
constexpr auto ALWAYS_WARMUP = std::chrono::seconds{5};
constexpr uint32_t SMART_SLOW_WINDOWS = 15;     // consecutive one-second windows below the refresh rate
constexpr double SMART_SLOW_RATIO = 0.92;       // below about 66 fps at 72 Hz
constexpr double SMART_MAX_RUNNING_RATIO = 2.6; // slower than this is a load or hitch, not gameplay
constexpr uint32_t VERIFY_LOCKED_WINDOWS = 3;
constexpr uint32_t VERIFY_MAX_WINDOWS = 6;
constexpr uint32_t FORCED_UNLOCKED_WINDOWS = 10;

enum class CliResult {
    NotFound,
    NeedsElevation,
    Failed,
    Ran,
};

std::optional<std::wstring> find_cli() {
    std::vector<std::wstring> candidates{};

    wchar_t base[MAX_PATH]{};
    const auto len = GetEnvironmentVariableW(L"OculusBase", base, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::wstring b{base};
        if (!b.empty() && b.back() != L'\\') {
            b += L'\\';
        }
        candidates.push_back(b + L"Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");
    }

    candidates.push_back(L"C:\\Program Files\\Oculus\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");
    candidates.push_back(L"C:\\Program Files\\Meta Horizon\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe");

    for (const auto& c : candidates) {
        const auto attr = GetFileAttributesW(c.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return c;
        }
    }

    return std::nullopt;
}

CliResult run_cli(const char* command) {
    const auto exe = find_cli();
    if (!exe) {
        spdlog::info("[MetaASW] Meta debug CLI not found");
        return CliResult::NotFound;
    }

    wchar_t temp_dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        return CliResult::Failed;
    }

    const std::wstring script = std::wstring{temp_dir} + L"uevr_meta_asw_commands.txt";
    {
        std::ofstream out{script, std::ios::trunc};
        if (!out) {
            return CliResult::Failed;
        }
        out << command << "\nexit\n"; // text mode writes CRLF
    }

    std::wstring cmdline = L"\"" + *exe + L"\" -f \"" + script + L"\"";
    const auto dir = exe->substr(0, exe->find_last_of(L'\\'));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(exe->c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            dir.c_str(), &si, &pi))
    {
        const auto err = GetLastError();
        spdlog::info("[MetaASW] Meta debug CLI did not start (error {}{})", err,
            err == ERROR_ELEVATION_REQUIRED ? ", needs administrator rights" : "");
        return err == ERROR_ELEVATION_REQUIRED ? CliResult::NeedsElevation : CliResult::Failed;
    }

    const auto waited = WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 0;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        spdlog::warn("[MetaASW] Meta debug CLI timed out running '{}'", command);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        spdlog::info("[MetaASW] Meta debug CLI ran '{}' (exit code {})", command, code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return waited == WAIT_TIMEOUT ? CliResult::Failed : CliResult::Ran;
}

// Ctrl+Numpad3 forces ASW, Ctrl+Numpad4 returns to automatic (Meta PC runtime shortcuts).
bool send_ctrl_numpad(WORD vk) {
    INPUT down[2]{};
    down[0].type = INPUT_KEYBOARD;
    down[0].ki.wVk = VK_CONTROL;
    down[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    down[1].type = INPUT_KEYBOARD;
    down[1].ki.wVk = vk;
    down[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));

    INPUT up[2]{down[1], down[0]};
    up[0].ki.dwFlags = KEYEVENTF_KEYUP;
    up[1].ki.dwFlags = KEYEVENTF_KEYUP;

    const auto sent_down = SendInput(2, down, sizeof(INPUT));
    Sleep(40);
    const auto sent_up = SendInput(2, up, sizeof(INPUT));
    return sent_down == 2 && sent_up == 2;
}

const char* method_name(int32_t m) {
    switch (m) {
    case 1: return "Meta debug CLI";
    case 2: return "keyboard shortcut";
    default: return "none";
    }
}
} // namespace

Controller& Controller::get() {
    // Never destroyed: the process-exit restore runs from DllMain after other statics may be gone.
    static auto* instance = new Controller{};
    return *instance;
}

const char* Controller::state_name() const {
    switch (m_state.load(std::memory_order_acquire)) {
    case State::Idle: return "watching";
    case State::Applying: return "switching";
    case State::Verifying: return "checking";
    case State::Forced: return "ASW forced";
    case State::Failed: return "could not force ASW";
    case State::Restored: return "set back to automatic";
    default: return "unknown";
    }
}

void Controller::set_runtime_name(std::string_view name) {
    std::string lower{name};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const bool meta = lower.find("oculus") != std::string::npos || lower.find("meta") != std::string::npos;

    {
        std::scoped_lock _{m_mtx};
        m_runtime_name = std::string{name};
    }

    m_is_meta.store(meta, std::memory_order_release);
    spdlog::info("[MetaASW] OpenXR runtime '{}': {}", std::string{name},
        meta ? "Meta PC runtime, ASW control available" : "not Meta's runtime, ASW control off");
}

void Controller::reset_window() {
    m_window_sum = 0;
    m_window_frames = 0;
    m_window_half_rate = 0;
}

void Controller::start_force(Method method) {
    bool expected = false;
    if (!m_worker_busy.compare_exchange_strong(expected, true)) {
        return;
    }

    m_state.store(State::Applying, std::memory_order_release);

    std::thread{[this, method]() {
        auto used = method;

        if (used == Method::Cli) {
            if (run_cli("server:asw.Sim45") != CliResult::Ran) {
                m_cli_unavailable.store(true, std::memory_order_release);
                used = Method::Hotkey;
            }
        }

        if (used == Method::Hotkey) {
            const bool sent = send_ctrl_numpad(VK_NUMPAD3);
            spdlog::info("[MetaASW] Sent Ctrl+Numpad3 (force ASW){}", sent ? "" : " - input was blocked");
        }

        {
            std::scoped_lock _{m_mtx};
            m_method.store(used, std::memory_order_release);
            m_applied_at = Clock::now();
            m_verify_windows = 0;
            m_locked_windows = 0;
            m_state.store(State::Verifying, std::memory_order_release);
        }

        m_worker_busy.store(false, std::memory_order_release);
    }}.detach();
}

void Controller::start_restore(bool wait) {
    const auto state = m_state.load(std::memory_order_acquire);
    if (state != State::Forced && state != State::Verifying) {
        return;
    }

    bool expected = false;
    if (!m_worker_busy.compare_exchange_strong(expected, true)) {
        return;
    }

    m_state.store(State::Applying, std::memory_order_release);
    const auto method = m_method.load(std::memory_order_acquire);

    auto work = [this, method]() {
        bool done = false;
        if (method == Method::Cli && !m_cli_unavailable.load(std::memory_order_acquire)) {
            done = run_cli("server:asw.Auto") == CliResult::Ran;
        }
        if (!done) {
            send_ctrl_numpad(VK_NUMPAD4);
        }
        spdlog::info("[MetaASW] ASW set back to automatic ({})", done ? "Meta debug CLI" : "Ctrl+Numpad4");
        m_state.store(State::Restored, std::memory_order_release);
        m_worker_busy.store(false, std::memory_order_release);
    };

    if (wait) {
        work();
    } else {
        std::thread{work}.detach();
    }
}

void Controller::on_wait_frame(int64_t t, int64_t period, bool focused, Mode mode) {
    if (!m_is_meta.load(std::memory_order_acquire)) {
        return;
    }

    std::scoped_lock _{m_mtx};
    const auto now = Clock::now();

    if (mode == Mode::LeaveAlone) {
        const auto state = m_state.load(std::memory_order_acquire);
        if (state == State::Forced || state == State::Verifying) {
            spdlog::info("[MetaASW] Mode changed to 'Leave alone'");
            start_restore(false);
        }
        m_last_display_time = t;
        return;
    }

    if (period > 0 && (m_min_period == 0 || period < m_min_period)) {
        m_min_period = period;
    }

    if (!focused) {
        m_was_focused = false;
        m_last_display_time = t;
        m_slow_streak = 0;
        reset_window();
        return;
    }

    if (!m_was_focused) {
        m_was_focused = true;
        m_focused_since = now;
    }

    if (m_last_display_time == 0 || t <= m_last_display_time || m_min_period <= 0) {
        m_last_display_time = t;
        return;
    }

    const auto delta = t - m_last_display_time;
    m_last_display_time = t;

    if (delta > 20 * m_min_period) {
        // Paused, loading or a long hitch: start over.
        m_slow_streak = 0;
        reset_window();
        return;
    }

    m_window_sum += delta;
    ++m_window_frames;
    if (std::llabs(delta - 2 * m_min_period) <= m_min_period * 15 / 100) {
        ++m_window_half_rate;
    }

    if (m_window_sum < 1'000'000'000) {
        return;
    }

    const auto avg = static_cast<double>(m_window_sum) / m_window_frames;
    const auto min_period = static_cast<double>(m_min_period);
    const auto ratio = min_period / avg;
    const auto half_fraction = static_cast<double>(m_window_half_rate) / m_window_frames;
    const bool locked = std::abs(avg - 2.0 * min_period) <= 0.1 * min_period && half_fraction >= 0.8;
    reset_window();

    switch (m_state.load(std::memory_order_acquire)) {
    case State::Idle: {
        const auto since = now - m_focused_since;

        if (mode == Mode::AlwaysForce) {
            if (since >= ALWAYS_WARMUP) {
                spdlog::info("[MetaASW] Mode 'Always force': forcing ASW");
                start_force(m_cli_unavailable.load() ? Method::Hotkey : Method::Cli);
            }
            break;
        }

        if (since < SMART_WARMUP) {
            break;
        }

        const bool slow = ratio < SMART_SLOW_RATIO && avg <= SMART_MAX_RUNNING_RATIO * min_period;
        m_slow_streak = slow ? m_slow_streak + 1 : 0;

        if (m_slow_streak >= SMART_SLOW_WINDOWS) {
            spdlog::info("[MetaASW] Smart: {:.1f} of {:.1f} fps for {}s; forcing ASW for an even half rate",
                1e9 / avg, 1e9 / min_period, m_slow_streak);
            m_slow_streak = 0;
            start_force(m_cli_unavailable.load() ? Method::Hotkey : Method::Cli);
        }
        break;
    }
    case State::Verifying: {
        if (now - m_applied_at < std::chrono::seconds{2}) {
            break;
        }

        ++m_verify_windows;
        if (locked) {
            ++m_locked_windows;
        }

        const auto method = m_method.load(std::memory_order_acquire);
        if (m_locked_windows >= VERIFY_LOCKED_WINDOWS) {
            m_state.store(State::Forced, std::memory_order_release);
            m_unlocked_windows = 0;
            spdlog::info("[MetaASW] ASW is forced: steady {:.1f} fps ({})", 1e9 / avg, method_name((int32_t)method));
        } else if (m_verify_windows >= VERIFY_MAX_WINDOWS) {
            if (method == Method::Cli) {
                spdlog::info("[MetaASW] Meta debug CLI did not change the frame cadence; trying the keyboard shortcut");
                m_cli_unavailable.store(true, std::memory_order_release);
                m_state.store(State::Idle, std::memory_order_release);
                start_force(Method::Hotkey);
            } else {
                m_state.store(State::Failed, std::memory_order_release);
                spdlog::warn("[MetaASW] Could not force ASW (cadence {:.1f} fps); leaving the runtime setting alone this session",
                    1e9 / avg);
            }
        }
        break;
    }
    case State::Forced:
        // Forced ASW caps the game at half rate. A full-rate cadence means the setting was changed outside UEVR.
        if (!locked && ratio > 0.75) {
            if (++m_unlocked_windows >= FORCED_UNLOCKED_WINDOWS) {
                m_state.store(State::Restored, std::memory_order_release);
                spdlog::info("[MetaASW] ASW was changed outside UEVR; leaving it alone this session");
            }
        } else {
            m_unlocked_windows = 0;
        }
        break;
    default:
        break;
    }
}

void Controller::on_session_stopping() {
    if (!m_is_meta.load(std::memory_order_acquire)) {
        return;
    }
    start_restore(false);
}

void Controller::on_window_destroyed() {
    if (!m_is_meta.load(std::memory_order_acquire)) {
        return;
    }
    start_restore(true);
}

void Controller::on_process_exit() {
    const auto state = m_state.load(std::memory_order_acquire);
    if (state != State::Forced && state != State::Verifying) {
        return;
    }
    // Loader lock is held here: no threads, no processes, just the runtime's shortcut.
    send_ctrl_numpad(VK_NUMPAD4);
    m_state.store(State::Restored, std::memory_order_release);
}

} // namespace uevr::meta_asw
