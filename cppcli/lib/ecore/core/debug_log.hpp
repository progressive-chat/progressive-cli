// src/core/debug_log.hpp — Qt-free debug logging, asserts, and function tracing.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

//
// Compile-time toggles (set via CMake -D flags, optional):
//   PROGRESSIVE_DISABLE_ASSERT  — removes all PROGRESSIVE_ASSERT checks
//   PROGRESSIVE_DISABLE_LOG     — removes all LOG() output
// By default (no flags): everything is ON for debug builds.
//

#ifndef PROGRESSIVE_DISABLE_ASSERT
#define PROGRESSIVE_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "[ASSERT] %s:%d: FAILED %s — %s\n", \
                         __FILE__, __LINE__, #cond, msg); \
            std::abort(); \
        } \
    } while(0)
#else
#define PROGRESSIVE_ASSERT(cond, msg) ((void)0)
#endif

enum class LogChannel {
    GUI,
    SYNC,
    E2EE,
    NET,
    MEM,
    DBG
};

#ifndef PROGRESSIVE_DISABLE_LOG
#define LOG(ch, fmt, ...) \
    do { \
        char pdcBuf[1024]; \
        int pdcLen = std::snprintf(pdcBuf, sizeof(pdcBuf), fmt, ##__VA_ARGS__); \
        if (pdcLen < 0) pdcLen = 0; \
        if (pdcLen > (int)sizeof(pdcBuf) - 1) pdcLen = (int)sizeof(pdcBuf) - 1; \
        pdcBuf[pdcLen] = '\0'; \
        switch (ch) { \
            case LogChannel::GUI:  std::fprintf(stderr, "[GUI]  %s\n", pdcBuf); break; \
            case LogChannel::SYNC: std::fprintf(stderr, "[SYNC] %s\n", pdcBuf); break; \
            case LogChannel::E2EE: std::fprintf(stderr, "[E2EE] %s\n", pdcBuf); break; \
            case LogChannel::NET:  std::fprintf(stderr, "[NET]  %s\n", pdcBuf); break; \
            case LogChannel::MEM:  std::fprintf(stderr, "[MEM]  %s\n", pdcBuf); break; \
            case LogChannel::DBG:  std::fprintf(stderr, "[DBG]  %s\n", pdcBuf); break; \
        } \
        ::progressive::logToRing(ch, std::string(pdcBuf)); \
    } while(0)
#else
#define LOG(ch, fmt, ...) ((void)0)
#endif

//
// In-app log ring buffer (Qt-free). The LOG() macro mirrors every line here;
// the UI log viewer (LogViewerDialog) shows this instead of the console.
//
namespace progressive {

struct LogLine {
    LogChannel channel;
    std::string text;
    uint64_t seq = 0;  // insertion order (for stable filtering)
};

// Single shared ring (one static per program for inline functions).
// BUG FIX (2026-08-06): logToRing and snapshotLogRing previously declared
// SEPARATE static deques — snapshot always read an empty ring, so the in-app
// log viewer could never show lines.
inline std::deque<LogLine>& logRing() {
    static std::deque<LogLine> ring;
    return ring;
}
inline std::mutex& logRingMtx() {
    static std::mutex mtx;
    return mtx;
}

inline void logToRing(LogChannel ch, const std::string& text) {
    static uint64_t seq = 0;
    std::lock_guard<std::mutex> lock(logRingMtx());
    auto& ring = logRing();
    ring.push_back({ch, text, seq++});
    if (ring.size() > 2000) ring.pop_front();
}

inline std::vector<LogLine> snapshotLogRing() {
    std::lock_guard<std::mutex> lock(logRingMtx());
    const auto& ring = logRing();
    return std::vector<LogLine>(ring.begin(), ring.end());
}

inline const char* logChannelName(LogChannel ch) {
    switch (ch) {
        case LogChannel::GUI:  return "GUI";
        case LogChannel::SYNC: return "SYNC";
        case LogChannel::E2EE: return "E2EE";
        case LogChannel::NET:  return "NET";
        case LogChannel::MEM:  return "MEM";
        case LogChannel::DBG:  return "DBG";
    }
    return "?";
}

}  // namespace progressive

class TraceFn {
    const char* name_;
public:
    explicit TraceFn(const char* name) : name_(name) {
        std::fprintf(stderr, "[TRACE] -> %s\n", name_);
    }
    ~TraceFn() {
        std::fprintf(stderr, "[TRACE] <- %s\n", name_);
    }
};
