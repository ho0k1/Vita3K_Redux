// Vita3K Redux - fork addition, see util/perf_hint.h
#include <util/perf_hint.h>

#ifdef __ANDROID__
#include <util/log.h>

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace perf_hint {
namespace {
constexpr int64_t target_ns = 16'666'667;
constexpr size_t max_threads = 12;

struct Api {
    void *(*get_manager)() = nullptr;
    void *(*create_session)(void *, const int32_t *, size_t, int64_t) = nullptr;
    int (*report)(void *, int64_t) = nullptr;
    int (*set_threads)(void *, const pid_t *, size_t) = nullptr; // API 34
    int64_t (*update_rate)(void *) = nullptr;                    // -1: the device has no hint support at all
    void (*close)(void *) = nullptr;
    bool loaded = false;
};

std::mutex g_mutex;
Api g_api;
void *g_session = nullptr;
std::vector<pid_t> g_tids;
bool g_unsupported = false; // the device refused a session once: no more attempts
std::atomic<bool> g_active{ false };
std::atomic<int> g_report_errors{ 0 };

void load_api() {
    if (g_api.loaded)
        return;
    g_api.loaded = true;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib)
        return;
    g_api.get_manager = reinterpret_cast<decltype(g_api.get_manager)>(dlsym(lib, "APerformanceHint_getManager"));
    g_api.create_session = reinterpret_cast<decltype(g_api.create_session)>(dlsym(lib, "APerformanceHint_createSession"));
    g_api.report = reinterpret_cast<decltype(g_api.report)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    g_api.set_threads = reinterpret_cast<decltype(g_api.set_threads)>(dlsym(lib, "APerformanceHint_setThreads"));
    g_api.update_rate = reinterpret_cast<decltype(g_api.update_rate)>(dlsym(lib, "APerformanceHint_getPreferredUpdateRateNanos"));
    g_api.close = reinterpret_cast<decltype(g_api.close)>(dlsym(lib, "APerformanceHint_closeSession"));
}

// g_mutex held
void rebuild_session() {
    load_api();
    if (g_unsupported || !g_api.get_manager || !g_api.create_session || !g_api.report || g_tids.empty())
        return;
    if (g_session && g_api.set_threads) {
        if (g_api.set_threads(g_session, g_tids.data(), g_tids.size()) == 0)
            return;
    }
    void *manager = g_api.get_manager();
    if (!manager)
        return;
    if (g_session && g_api.close) {
        g_api.close(g_session);
        g_session = nullptr;
    }
    const int64_t rate = g_api.update_rate ? g_api.update_rate(manager) : -2;
    std::vector<int32_t> tids(g_tids.begin(), g_tids.end());
    g_session = g_api.create_session(manager, tids.data(), tids.size(), target_ns);
    g_active.store(g_session != nullptr, std::memory_order_release);
    if (!g_session && g_tids.size() >= 2) {
        // refused with the render thread and one more: the device (HAL) does not offer sessions to apps, stop trying
        g_unsupported = true;
    }
    LOG_INFO("[PERF-HINT] performance hint session {} for {} thread(s), target {:.1f} ms, preferred update rate {} ns (-1 = no device support, -2 = unknown){}{}",
        g_session ? "created" : "NOT available", tids.size(), target_ns / 1e6, rate, g_api.set_threads ? "" : " (threads cannot be added later on this Android version)",
        g_unsupported ? "; giving up" : "");
}
} // namespace

void add_current_thread() {
    const pid_t tid = gettid();
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tids.size() >= max_threads || std::find(g_tids.begin(), g_tids.end(), tid) != g_tids.end())
        return;
    // without setThreads the session is fixed after the first creation: the first threads (render, main game thread) win
    if (g_session && !g_api.set_threads)
        return;
    g_tids.push_back(tid);
    rebuild_session();
}

void report_frame(int64_t actual_ns) {
    if (!g_active.load(std::memory_order_acquire) || actual_ns <= 0)
        return;
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_session)
        return;
    const int rc = g_api.report(g_session, actual_ns);
    if (rc != 0 && g_report_errors.fetch_add(1) < 3)
        LOG_WARN("[PERF-HINT] reportActualWorkDuration failed: {}", rc);
}

void shutdown() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session && g_api.close)
        g_api.close(g_session);
    g_session = nullptr;
    g_tids.clear();
    g_active.store(false, std::memory_order_release);
}
} // namespace perf_hint
#else
namespace perf_hint {
void add_current_thread() {}
void report_frame(int64_t) {}
void shutdown() {}
} // namespace perf_hint
#endif
