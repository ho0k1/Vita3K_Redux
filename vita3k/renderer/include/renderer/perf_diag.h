// Lightweight stall diagnostics (docs/PERFORMANCE_PLAN.md, stage 0).
// Everything here only measures and writes to the normal log; no behaviour changes.
#pragma once

#include <vector>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace renderer::perf {

using Clock = std::chrono::steady_clock;

inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// accumulated microseconds / counts, written by any thread
struct Totals {
    std::atomic<uint64_t> shader_us{ 0 };
    std::atomic<uint64_t> pipeline_us{ 0 };
    std::atomic<uint64_t> world_stop_us{ 0 };
    std::atomic<uint32_t> shaders{ 0 };
    std::atomic<uint32_t> pipelines{ 0 };
    std::atomic<uint32_t> world_stops{ 0 };
    std::atomic<uint32_t> slow_shaders{ 0 }; // >= 5 ms
    std::atomic<uint32_t> slow_pipelines{ 0 }; // >= 5 ms
};

inline Totals &totals() {
    static Totals t;
    return t;
}

// set once by the render thread so that stalls are attributed to the right place
inline bool &on_render_thread() {
    static thread_local bool v = false;
    return v;
}

// microseconds spent compiling on the render thread (shaders + pipelines) since the last frame
inline std::atomic<uint64_t> &render_thread_compile_us() {
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

enum class Kind { Shader,
    Pipeline };

// why the render thread has to compile right now (set just before a synchronous compile, shown in the slow-compile log)
inline const char *&sync_reason() {
    static thread_local const char *reason = "async";
    return reason;
}

// Measures one compile; anything >= 5 ms is logged with the thread it ran on.
class CompileScope {
public:
    CompileScope(Kind kind, const char *what)
        : kind(kind)
        , what(what)
        , start(now_us()) {}
    ~CompileScope() {
        const uint64_t us = now_us() - start;
        auto &t = totals();
        const bool render = on_render_thread();
        if (kind == Kind::Shader) {
            t.shader_us += us;
            ++t.shaders;
        } else {
            t.pipeline_us += us;
            ++t.pipelines;
        }
        if (render)
            render_thread_compile_us() += us;
        if (us >= 5000) {
            if (kind == Kind::Shader)
                ++t.slow_shaders;
            else
                ++t.slow_pipelines;
            LOG_INFO("[PERF] slow {} ({}, {}) {:.1f} ms on {} thread", kind == Kind::Shader ? "shader" : "pipeline", what, sync_reason(), us / 1000.0, render ? "RENDER" : "worker");
        }
    }

private:
    Kind kind;
    const char *what;
    uint64_t start;
};

inline void note_world_stop(uint64_t us) {
    auto &t = totals();
    t.world_stop_us += us;
    ++t.world_stops;
}

// Per presented frame (render thread). Logs frames that took too long with a breakdown,
// plus a summary every 10 s.
class FrameMeter {
public:
    void begin_batches() { t_begin = now_us(); }
    void end_batches() { t_batches = now_us(); }
    void end_render() { t_render = now_us(); }
    void end_present() {
        t_end = now_us();
        if (t_begin == 0)
            return;
        // time between two shown frames: with the frame generator an even 16.7 ms and not 8/25 ms pairs
        if (last_present_us != 0 && pace_intervals.size() < 4096)
            pace_intervals.push_back(static_cast<uint32_t>(std::min<uint64_t>(t_end - last_present_us, 1'000'000)));
        last_present_us = t_end;
        const uint64_t total = t_end - t_begin;
        const uint64_t batches = t_batches - t_begin;
        const uint64_t render = t_render - t_batches;
        const uint64_t present = t_end - t_render;
        const uint64_t compile = render_thread_compile_us().exchange(0);
        auto &t = totals();
        const uint64_t ws = t.world_stop_us.load();
        const uint64_t ws_delta = ws - last_world_stop_us;
        last_world_stop_us = ws;

        ++frames;
        sum_us += total;
        max_us = std::max(max_us, total);
        const size_t bucket = std::min<size_t>(total / 5000, buckets.size() - 1); // 5 ms buckets
        ++buckets[bucket];
        if (total >= 33000)
            ++over33;
        if (total >= 50000)
            ++over50;
        if (total >= 100000)
            ++over100;
        compile_sum_us += compile;

        if (total >= 45000) {
            // batches = waiting for / executing guest commands (includes compiles), render = recording and submitting
            // the display frame, present = swapchain acquire/present (vsync waits show up here)
            LOG_INFO("[PERF] long frame {:.1f} ms: batches {:.1f} (compile on render thread {:.1f}), render {:.1f}, present {:.1f}, world-stop {:.1f}",
                total / 1000.0, batches / 1000.0, compile / 1000.0, render / 1000.0, present / 1000.0, ws_delta / 1000.0);
        }

        const uint64_t now = t_end;
        if (last_summary == 0)
            last_summary = now;
        if (now - last_summary >= 10'000'000) {
            summary(now - last_summary);
            last_summary = now;
        }
    }

private:
    void summary(uint64_t window_us) {
        uint32_t p95 = 0;
        {
            const uint32_t need = frames - frames / 20; // 95 % of frames
            uint32_t acc = 0;
            for (size_t i = 0; i < buckets.size(); ++i) {
                acc += buckets[i];
                if (acc >= need) {
                    p95 = static_cast<uint32_t>((i + 1) * 5);
                    break;
                }
            }
        }
        if (pace_intervals.size() >= 30) {
            std::sort(pace_intervals.begin(), pace_intervals.end());
            const uint32_t median = pace_intervals[pace_intervals.size() / 2];
            const uint32_t p99 = pace_intervals[std::min(pace_intervals.size() - 1, pace_intervals.size() * 99 / 100)];
            // an interval more than 40% away from the median is felt as a stutter
            size_t irregular = 0;
            for (const uint32_t v : pace_intervals)
                if (v > median * 1.4 || v < median * 0.6)
                    ++irregular;
            LOG_INFO("[PERF] pacing: {} shown frames, median interval {:.1f} ms, p99 {:.1f} ms, irregular {} ({:.1f}%)",
                pace_intervals.size(), median / 1000.0, p99 / 1000.0, irregular, 100.0 * irregular / pace_intervals.size());
        }
        pace_intervals.clear();
        auto &t = totals();
        LOG_INFO("[PERF] 10s: {} frames ({:.1f}/s), avg {:.1f} ms, p95 <= {} ms, max {:.1f} ms, >=33ms {}, >=50ms {}, >=100ms {} | compiles: shaders {} ({:.0f} ms, {} slow), pipelines {} ({:.0f} ms, {} slow), render-thread compile {:.0f} ms | world-stops {} ({:.0f} ms)",
            frames, frames * 1e6 / double(window_us), frames ? sum_us / 1000.0 / frames : 0.0, p95, max_us / 1000.0, over33, over50, over100,
            t.shaders.exchange(0), t.shader_us.exchange(0) / 1000.0, t.slow_shaders.exchange(0),
            t.pipelines.exchange(0), t.pipeline_us.exchange(0) / 1000.0, t.slow_pipelines.exchange(0),
            compile_sum_us / 1000.0, t.world_stops.exchange(0), (t.world_stop_us.load() - window_ws_base) / 1000.0);
        window_ws_base = t.world_stop_us.load();
        frames = over33 = over50 = over100 = 0;
        sum_us = max_us = compile_sum_us = 0;
        buckets.fill(0);
    }

    uint64_t last_present_us = 0;
    std::vector<uint32_t> pace_intervals;
    uint64_t t_begin = 0, t_batches = 0, t_render = 0, t_end = 0;
    uint64_t last_summary = 0;
    uint64_t last_world_stop_us = 0, window_ws_base = 0;
    uint32_t frames = 0, over33 = 0, over50 = 0, over100 = 0;
    uint64_t sum_us = 0, max_us = 0, compile_sum_us = 0;
    std::array<uint32_t, 40> buckets{}; // 0..200 ms
};

} // namespace renderer::perf
