// android/log.h — desktop shim
// Replaces Android NDK <android/log.h> so progressive_native modules
// (eventdb.cpp, olm_session.cpp, sas_verification.cpp, etc.) compile
// unchanged on Linux desktop. Maps __android_log_print -> fprintf(stderr).
//
// This is NOT part of the public API. It exists only to keep the
// progressive_native source tree bit-identical across Android and desktop.
#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstdint>

enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
};

static inline const char* __pdesktop_log_prio(int prio) {
    switch (prio) {
        case ANDROID_LOG_VERBOSE: return "V";
        case ANDROID_LOG_DEBUG:   return "D";
        case ANDROID_LOG_INFO:    return "I";
        case ANDROID_LOG_WARN:    return "W";
        case ANDROID_LOG_ERROR:   return "E";
        case ANDROID_LOG_FATAL:   return "F";
        default:                  return "?";
    }
}

static inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    std::fprintf(stderr, "[%s/%s] ", __pdesktop_log_prio(prio), tag ? tag : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return 0;
}

static inline int __android_log_write(int prio, const char* tag, const char* text) {
    return __android_log_print(prio, tag, "%s", text ? text : "(null)");
}
