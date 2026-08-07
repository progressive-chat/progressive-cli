// src/core/memory_stats.hpp — memory diagnostics via /proc/self/status + malloc_info.
// Non-invasive — only adds logging, never changes behaviour.
#pragma once

namespace progressive::desktop {

struct MemSnapshot {
    long vmRSS_kb = 0;      // resident set size — real RAM used by this process
    long vmPeak_kb = 0;     // peak RSS since process start
    long vmSize_kb = 0;     // virtual memory size
    long heapUsed_kb = 0;   // bytes actually in use (glibc uordblks)
    long heapFree_kb = 0;   // bytes free inside the heap (glibc fordblks)
    long heapTotal_kb = 0;  // arena size = used + free
};

// Take a snapshot of the current process memory usage.
// Returns all-zero on failure (no /proc filesystem, etc.).
MemSnapshot takeMemorySnapshot();

// Convenience: log a labelled snapshot to stderr.
// Format:  [mem] label: RSS=12345 KB peak=..., heap=used/free (total=X) KB
void logMemorySnapshot(const char* label);

// Print sizeof() for key data structures to stderr.
void logStructSizes();

// Trigger malloc_trim — asks glibc to return unused heap pages to the OS.
// Returns 1 on success, 0 on failure/no-op.
int trimMemory();

} // namespace progressive::desktop
