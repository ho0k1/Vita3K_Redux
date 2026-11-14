// Vita3K emulator project - fork addition
#include <cstring>
#include <util/session_diag.h>

#include <util/log.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace session_diag {

namespace {

constexpr size_t TABLE_SIZE = 8192; // power of two, a few thousand distinct imports exist at most
constexpr size_t MAX_PROBE = 64;

struct Slot {
    std::atomic<uint32_t> nid{ 0 };
    std::atomic<uint64_t> calls{ 0 };
    std::atomic<uint64_t> errors{ 0 };
    std::atomic<uint32_t> last_error{ 0 };
};

std::array<Slot, TABLE_SIZE> g_slots;
std::atomic<uint64_t> g_total_calls{ 0 };
std::atomic<uint64_t> g_total_errors{ 0 };
std::atomic<uint64_t> g_overflow_calls{ 0 };

std::mutex g_sites_mutex;
std::vector<StubSite *> g_sites;

std::mutex g_hook_mutex;
std::function<void(const char *)> g_hook;

Slot *find_slot(uint32_t nid) {
    size_t index = (static_cast<size_t>(nid) * 2654435761u >> 7) & (TABLE_SIZE - 1);
    for (size_t probe = 0; probe < MAX_PROBE; probe++) {
        Slot &slot = g_slots[(index + probe) & (TABLE_SIZE - 1)];
        uint32_t current = slot.nid.load(std::memory_order_relaxed);
        if (current == nid)
            return &slot;
        if (current == 0) {
            if (slot.nid.compare_exchange_strong(current, nid, std::memory_order_relaxed) || current == nid)
                return &slot;
        }
    }
    return nullptr;
}

} // namespace

bool note_call(uint32_t nid, uint32_t ret) {
    g_total_calls.fetch_add(1, std::memory_order_relaxed);
    if (nid == 0)
        nid = 1;
    Slot *slot = find_slot(nid);
    if (!slot) {
        g_overflow_calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slot->calls.fetch_add(1, std::memory_order_relaxed);
    // SCE error codes are 0x80xxxxxx; guest addresses start at 0x81000000, so pointers do not count as errors
    if ((ret >> 24) != 0x80)
        return false;
    g_total_errors.fetch_add(1, std::memory_order_relaxed);
    slot->last_error.store(ret, std::memory_order_relaxed);
    return slot->errors.fetch_add(1, std::memory_order_relaxed) == 0;
}

static std::atomic<int> g_thermal_status{ -1 };
void set_thermal_status(int status) {
    g_thermal_status.store(status, std::memory_order_relaxed);
}
int thermal_status() {
    return g_thermal_status.load(std::memory_order_relaxed);
}

bool returns_pointer(const char *name) {
    if (!name)
        return false;
    static const char *const prefixes[] = { "sceClibMem", "sceClibStr", "sceClibMspace", "sceClibPrintf", "sceClibVsnprintf",
        "sceClibSnprintf", "kmem", "sceKernelGetTLSAddr", "sceNetErrnoLoc", "sceSysclib", "memcpy", "memset", "memmove", "strn", "strc", "strl" };
    for (const char *prefix : prefixes) {
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0)
            return true;
    }
    return false;
}

uint64_t total_calls() {
    return g_total_calls.load(std::memory_order_relaxed);
}

uint64_t total_errors() {
    return g_total_errors.load(std::memory_order_relaxed);
}

void StubSite::hit() {
    count.fetch_add(1, std::memory_order_relaxed);
    if (!registered.test_and_set()) {
        const std::lock_guard<std::mutex> lock(g_sites_mutex);
        g_sites.push_back(this);
    }
}

void reset() {
    // the slots keep their NID keys (the table only grows), only the numbers are cleared
    for (Slot &slot : g_slots) {
        slot.calls.store(0, std::memory_order_relaxed);
        slot.errors.store(0, std::memory_order_relaxed);
        slot.last_error.store(0, std::memory_order_relaxed);
    }
    g_total_calls.store(0, std::memory_order_relaxed);
    g_total_errors.store(0, std::memory_order_relaxed);
    g_overflow_calls.store(0, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(g_sites_mutex);
    for (StubSite *site : g_sites)
        site->count.store(0, std::memory_order_relaxed);
}

void log_summary(const char *reason, const std::function<NidName(uint32_t)> &namer) {
    struct Row {
        uint32_t nid;
        uint64_t calls;
        uint64_t errors;
        uint32_t last_error;
        NidName name;
    };
    std::vector<Row> rows;
    for (Slot &slot : g_slots) {
        const uint32_t nid = slot.nid.load(std::memory_order_relaxed);
        const uint64_t calls = slot.calls.load(std::memory_order_relaxed);
        if (nid == 0 || calls == 0)
            continue;
        Row row{ nid, calls, slot.errors.load(std::memory_order_relaxed), slot.last_error.load(std::memory_order_relaxed), {} };
        row.name = namer ? namer(nid) : NidName{};
        if (row.name.function.empty())
            row.name.function = fmt::format("0x{:08X}", nid);
        if (row.name.module.empty())
            row.name.module = "?";
        rows.push_back(std::move(row));
    }

    LOG_INFO("[CALLS:{}] {} HLE calls, {} distinct functions, {} error-like returns (0x80xxxxxx){}", reason, g_total_calls.load(), rows.size(),
        g_total_errors.load(), g_overflow_calls.load() ? fmt::format(", {} calls not counted (table full)", g_overflow_calls.load()) : std::string());

    // most called
    std::vector<const Row *> by_calls;
    for (const Row &row : rows)
        by_calls.push_back(&row);
    std::sort(by_calls.begin(), by_calls.end(), [](const Row *a, const Row *b) { return a->calls > b->calls; });
    for (size_t i = 0; i < by_calls.size() && i < 15; i++)
        LOG_INFO("[CALLS:{}] top {:>2}: {} ({}) x{}", reason, i + 1, by_calls[i]->name.function, by_calls[i]->name.module, by_calls[i]->calls);

    // per module: calls and errors
    struct ModuleRow {
        uint64_t calls = 0;
        uint64_t errors = 0;
    };
    std::map<std::string, ModuleRow> modules;
    for (const Row &row : rows) {
        modules[row.name.module].calls += row.calls;
        modules[row.name.module].errors += row.errors;
    }
    for (const auto &[module, row] : modules) {
        if (row.errors)
            LOG_WARN("[MODULE:{}] {}: {} calls, {} error-like returns", reason, module, row.calls, row.errors);
        else
            LOG_INFO("[MODULE:{}] {}: {} calls, no errors", reason, module, row.calls);
    }

    // functions that returned errors
    std::vector<const Row *> failing;
    for (const Row &row : rows)
        if (row.errors)
            failing.push_back(&row);
    std::sort(failing.begin(), failing.end(), [](const Row *a, const Row *b) { return a->errors > b->errors; });
    for (size_t i = 0; i < failing.size() && i < 40; i++)
        LOG_WARN("[ERRORS:{}] {} ({}) failed {} of {} calls, last code 0x{:08X}", reason, failing[i]->name.function, failing[i]->name.module,
            failing[i]->errors, failing[i]->calls, failing[i]->last_error);

    // unimplemented and stubbed functions the app hit
    std::vector<std::pair<std::string, uint64_t>> stubs;
    {
        const std::lock_guard<std::mutex> lock(g_sites_mutex);
        for (const StubSite *site : g_sites) {
            const uint64_t count = site->count.load(std::memory_order_relaxed);
            if (count)
                stubs.emplace_back(fmt::format("{} [{}]", site->name, site->kind), count);
        }
    }
    std::sort(stubs.begin(), stubs.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (stubs.empty())
        LOG_INFO("[STUBS:{}] no unimplemented or stubbed function was called", reason);
    for (const auto &[name, count] : stubs)
        LOG_WARN("[STUBS:{}] {} x{}", reason, name, count);
}

void set_report_hook(std::function<void(const char *)> hook) {
    const std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hook = std::move(hook);
}

void report(const char *reason) {
    std::function<void(const char *)> hook;
    {
        const std::lock_guard<std::mutex> lock(g_hook_mutex);
        hook = g_hook;
    }
    if (hook)
        hook(reason);
}

} // namespace session_diag
