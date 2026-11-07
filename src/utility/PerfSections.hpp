#pragma once

// Lightweight per-section timing for hitch diagnostics builds.
//
// Sections accumulates wall time per named section and logs averages per frame every few seconds.
// Callers pass string literals (or other strings that outlive the process) as section names.
// Each Sections instance must only be used from one thread at a time, or be guarded by the caller.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

namespace utility::perf {

using Clock = std::chrono::steady_clock;

inline double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

template <size_t N>
class Sections {
public:
    void add(const char* name, double ms) {
        for (auto& slot : m_slots) {
            if (slot.name == nullptr) {
                slot.name = name;
            } else if (slot.name != name && std::strcmp(slot.name, name) != 0) {
                continue;
            }

            slot.sum_ms += ms;
            slot.max_ms = (std::max)(slot.max_ms, ms);
            ++slot.count;
            return;
        }
    }

    void count_frame() { ++m_frames; }

    // Logs "<label> frames=N name=avg/max ..." once per interval. avg = total time per frame (ms),
    // max = longest single section call (ms). Resets after logging.
    void log_if_due(const char* label, std::chrono::seconds interval = std::chrono::seconds{5}) {
        const auto now = Clock::now();

        if (m_last_log.time_since_epoch().count() == 0) {
            m_last_log = now;
            return;
        }

        if (now - m_last_log < interval) {
            return;
        }

        std::string line{};
        line.reserve(512);

        const auto frames = m_frames > 0 ? (double)m_frames : 1.0;
        double total = 0.0;

        for (const auto& slot : m_slots) {
            if (slot.name == nullptr || slot.count == 0) {
                continue;
            }

            const auto per_frame = slot.sum_ms / frames;
            total += per_frame;

            char buf[160]{};
            std::snprintf(buf, sizeof(buf), " %s=%.3f/%.2f", slot.name, per_frame, slot.max_ms);
            line += buf;
        }

        spdlog::info("[Perf][{}] frames={} sum={:.3f}ms/frame (name=avg ms per frame/max ms per call){}", label, m_frames, total, line);

        for (auto& slot : m_slots) {
            slot.sum_ms = 0.0;
            slot.max_ms = 0.0;
            slot.count = 0;
        }

        m_frames = 0;
        m_last_log = now;
    }

    void reset() {
        m_slots = {};
        m_frames = 0;
        m_last_log = {};
    }

private:
    struct Slot {
        const char* name{nullptr};
        double sum_ms{0.0};
        double max_ms{0.0};
        uint32_t count{0};
    };

    std::array<Slot, N> m_slots{};
    uint64_t m_frames{0};
    Clock::time_point m_last_log{};
};

// Sequential checkpoints inside one call: each mark() adds the time since the previous mark
// (or since construction) to the named section. Does nothing when disabled.
template <size_t N>
class Checkpoints {
public:
    Checkpoints(Sections<N>& sections, bool enabled)
        : m_sections{sections}, m_enabled{enabled}, m_last{enabled ? Clock::now() : Clock::time_point{}} {}

    void mark(const char* name) {
        if (!m_enabled) {
            return;
        }

        const auto now = Clock::now();
        m_sections.add(name, ms_between(m_last, now));
        m_last = now;
    }

    bool enabled() const { return m_enabled; }

private:
    Sections<N>& m_sections;
    bool m_enabled{false};
    Clock::time_point m_last{};
};

} // namespace utility::perf
