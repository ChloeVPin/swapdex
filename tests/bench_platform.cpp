// Read-only timing of the platform probes the service runs while it is idle. Nothing
// here starts, stops, or changes a Codex process, so it is safe to run next to a live
// Codex session. Build with the swapdex_bench target and run it with an iteration count:
//   swapdex_bench.exe 200

#include "platform.hpp"

#include <cstdlib>
#include <cstdio>
#include <functional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <chrono>
#endif

namespace {

double now_microseconds() {
#if defined(_WIN32)
    LARGE_INTEGER frequency {};
    LARGE_INTEGER counter {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000000.0 / static_cast<double>(frequency.QuadPart);
#else
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

void measure(const char* name, int iterations, const std::function<void()>& probe) {
    probe();
    const double start = now_microseconds();
    for (int index = 0; index < iterations; ++index) {
        probe();
    }
    const double total = now_microseconds() - start;
    std::printf("%-32s iterations=%d avg_us=%.1f\n", name, iterations, total / iterations);
}

}

int main(int argc, char** argv) {
    const int requested = argc > 1 ? std::atoi(argv[1]) : 200;
    const int iterations = requested > 0 ? requested : 1;
    measure("find_chatgpt_binary", iterations, [] {
        volatile std::size_t length = swapdex::platform::find_chatgpt_binary().string().size();
        static_cast<void>(length);
    });
#if defined(_WIN32)
    // The floor for any process probe: one toolhelp snapshot walked end to end.
    measure("toolhelp_process_snapshot", iterations, [] {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::size_t count = 0;
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            ++count;
        }
        CloseHandle(snapshot);
        volatile std::size_t sink = count;
        static_cast<void>(sink);
    });
#endif
    measure("running_unmanaged_chatgpt", iterations, [] {
        volatile bool found = swapdex::platform::running_unmanaged_chatgpt(-1).has_value();
        static_cast<void>(found);
    });
    return 0;
}
