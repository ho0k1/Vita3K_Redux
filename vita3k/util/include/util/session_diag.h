// Vita3K emulator project - fork addition
// Per-session statistics about HLE calls: how often every function was called, which ones returned
// error-like values and which unimplemented / stubbed ones the app hit. Written into the session log
// at the end of a session, on a freeze and on a crash.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace session_diag {

// Returns true the first time this NID returned an error-like value (0x80xxxxxx, e.g. 0x80010002) in this session.
bool note_call(uint32_t nid, uint32_t ret);
// functions whose result is a pointer or a length, never an SCE error code (memcpy, strlen, TLS address ...)
bool returns_pointer(const char *name);
// Android thermal status (0 none ... 6 shutdown), -1 unknown
void set_thermal_status(int status);
int thermal_status();

// adaptive resolution: the scale the user chose and the one in use now (0 = not active), shown by the FPS overlay
void set_adaptive_resolution(float base, float current);
float adaptive_resolution_base();
float adaptive_resolution_current();

// A UNIMPLEMENTED()/STUBBED() call site; lives for the whole process (function-local static).
struct StubSite {
    const char *name;
    const char *kind; // "unimplemented" or "stubbed"
    std::atomic<uint64_t> count{ 0 };
    std::atomic_flag registered = ATOMIC_FLAG_INIT;
    StubSite(const char *n, const char *k)
        : name(n)
        , kind(k) {}
    void hit();
};

uint64_t total_calls();
uint64_t total_errors();

// Forgets the counters of the previous session (call at the start of a session).
void reset();

// Writes the call statistics to the log. namer gives "function" and "module" names of a NID.
struct NidName {
    std::string function;
    std::string module;
};
void log_summary(const char *reason, const std::function<NidName(uint32_t)> &namer);

// Anything that needs the emulator state (thread dumps, names) registers a hook; report() runs it.
void set_report_hook(std::function<void(const char *reason)> hook);
void report(const char *reason);

} // namespace session_diag
