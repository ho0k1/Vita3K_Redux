// Vita3K emulator project - fork addition
// Tracks "the guest app is busy with a download or an install" so that the Android frontend
// can keep the emulator running in the background (foreground service) instead of pausing it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace background_activity {

inline std::atomic<int64_t> last_net_rx_ms{ INT64_MIN / 2 };
inline std::atomic<int64_t> last_io_write_ms{ INT64_MIN / 2 };
inline std::atomic<uint64_t> net_rx_bytes{ 0 };
// number of installs (scePromoterUtility) running right now
inline std::atomic<int> promoting{ 0 };

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void note_net_rx(int64_t bytes) {
    if (bytes <= 0)
        return;
    net_rx_bytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
    last_net_rx_ms.store(now_ms(), std::memory_order_relaxed);
}

inline void note_io_write() {
    last_io_write_ms.store(now_ms(), std::memory_order_relaxed);
}

// true when data came in, files were written or an install is running within the last window_ms
inline bool is_active(int64_t window_ms) {
    if (promoting.load(std::memory_order_relaxed) > 0)
        return true;
    const int64_t now = now_ms();
    return now - last_net_rx_ms.load(std::memory_order_relaxed) < window_ms
        || now - last_io_write_ms.load(std::memory_order_relaxed) < window_ms;
}

struct PromoteScope {
    PromoteScope() { promoting.fetch_add(1, std::memory_order_relaxed); }
    ~PromoteScope() { promoting.fetch_sub(1, std::memory_order_relaxed); }
};

} // namespace background_activity
