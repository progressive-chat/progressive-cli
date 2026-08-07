// src/core/crash_handler.hpp — signal handler with backtrace for SIGSEGV/SIGABRT.
#pragma once
#include <cstdio>  // for fprintf in stub

#if defined(__linux__)
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>

namespace progressive::crash {

inline void installCrashHandler() {
    static auto handler = [](int sig) -> void {
        const char* name = "UNKNOWN";
        switch (sig) {
            case SIGSEGV: name = "SIGSEGV"; break;
            case SIGABRT: name = "SIGABRT"; break;
            case SIGFPE:  name = "SIGFPE";  break;
            case SIGILL:  name = "SIGILL";  break;
        }
        std::fprintf(stderr, "\n[CRASH] Signal %d (%s)\n", sig, name);

        void* buffer[32];
        int frames = backtrace(buffer, 32);
        char** symbols = backtrace_symbols(buffer, frames);
        if (symbols) {
            std::fprintf(stderr, "[BACKTRACE] %d frames:\n", frames);
            for (int i = 0; i < frames; ++i) {
                std::fprintf(stderr, "  #%d %s\n", i, symbols[i]);
            }
            std::free(symbols);
        } else {
            std::fprintf(stderr, "[BACKTRACE] no symbols (link with -rdynamic)\n");
        }

        std::fflush(stderr);
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    };

    std::signal(SIGSEGV, handler);
    std::signal(SIGABRT, handler);
    std::signal(SIGFPE,  handler);
    std::signal(SIGILL,  handler);
    // CLI deviation from the desktop original: no "handler installed" noise
    // on stderr — every matrixcli invocation would print it.
}

} // namespace

#else
namespace progressive::crash {
inline void installCrashHandler() {
    std::fprintf(stderr, "[crash] handler not installed (non-Linux platform)\n");
}
} // namespace
#endif
