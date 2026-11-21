// Vita3K Redux - fork addition
// Android performance hints (ADPF, NDK APerformanceHint, API 33+; threads can be added from API 34 on).
// The emulator tells the system which threads make up a frame and how long the frame took against a target, so that
// the scheduler keeps them on fast cores and raises the CPU clock before a slow frame, not after it. Loaded with
// dlsym, so older systems and other platforms simply do nothing.
#pragma once

#include <cstdint>

namespace perf_hint {

// Registers the calling (host) thread as part of the frame work. The first call creates the hint session.
void add_current_thread();
// Called once per presented frame by the render thread: how long the frame's work took, in nanoseconds.
void report_frame(int64_t actual_ns);
// Closes the session (end of the game session).
void shutdown();

} // namespace perf_hint
