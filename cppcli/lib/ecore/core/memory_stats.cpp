// src/core/memory_stats.cpp — reads /proc/self/status + mallinfo2 for diagnostics.

#include "memory_stats.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <malloc.h>
#endif

// forward-declare key structs for sizeof() logging
#include "../core/fast_sync.hpp"
#include "../core/crypto/olm_account.hpp"
#include "../core/crypto/megolm_store.hpp"
#include "../core/crypto/decryptor.hpp"

namespace progressive::desktop {

MemSnapshot takeMemorySnapshot() {
    MemSnapshot s;
#if defined(__linux__)
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) return s;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long val = 0;
            std::sscanf(line.c_str(), "VmRSS: %ld kB", &val);
            s.vmRSS_kb = val;
        } else if (line.rfind("VmPeak:", 0) == 0) {
            long val = 0;
            std::sscanf(line.c_str(), "VmPeak: %ld kB", &val);
            s.vmPeak_kb = val;
        } else if (line.rfind("VmSize:", 0) == 0) {
            long val = 0;
            std::sscanf(line.c_str(), "VmSize: %ld kB", &val);
            s.vmSize_kb = val;
        }
    }

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    s.heapUsed_kb = static_cast<long>(mi.uordblks / 1024);
    s.heapFree_kb = static_cast<long>(mi.fordblks / 1024);
    s.heapTotal_kb = static_cast<long>(mi.arena / 1024);
#else
    struct mallinfo mi = mallinfo();
    s.heapUsed_kb = static_cast<long>(mi.uordblks / 1024);
    s.heapFree_kb = static_cast<long>(mi.fordblks / 1024);
    s.heapTotal_kb = static_cast<long>(mi.arena / 1024);
#endif
#endif
    return s;
}

void logMemorySnapshot(const char* label) {
    auto s = takeMemorySnapshot();
    std::fprintf(stderr, "[mem] %s: RSS=%ld KB peak=%ld KB heap=%ld/%ld (total=%ld) KB\n",
                 label, s.vmRSS_kb, s.vmPeak_kb,
                 s.heapUsed_kb, s.heapFree_kb, s.heapTotal_kb);
}

void logStructSizes() {
    std::fprintf(stderr, "[mem] struct sizes (bytes):\n");
    std::fprintf(stderr, "  FastEvent              = %zu\n", sizeof(FastEvent));
    std::fprintf(stderr, "  FastRoomTimeline       = %zu\n", sizeof(FastRoomTimeline));
    std::fprintf(stderr, "  FastRoom               = %zu\n", sizeof(FastRoom));
    std::fprintf(stderr, "  FastSyncResponse       = %zu\n", sizeof(FastSyncResponse));
    std::fprintf(stderr, "  OlmAccountStore        = %zu\n", sizeof(OlmAccountStore));
    std::fprintf(stderr, "  MegolmStore            = %zu\n", sizeof(MegolmStore));
    std::fprintf(stderr, "  Decryptor              = %zu\n", sizeof(Decryptor));
    std::fprintf(stderr, "  OutboundMegolmSession  = %zu\n", sizeof(OutboundMegolmSession));
}

int trimMemory() {
#if defined(__linux__)
    return malloc_trim(0);
#else
    return 0;  // no-op on non-Linux
#endif
}

} // namespace progressive::desktop
