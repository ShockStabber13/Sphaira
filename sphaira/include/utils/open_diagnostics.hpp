#pragma once

// Debug-only information displayed when opening a remote file fails.
// Do not store credentials, URLs, or filenames here.
#include <atomic>

namespace sphaira::open_diagnostics {
inline std::atomic<int> file_errno{0};
inline std::atomic<bool> webdav_seen{false};
inline std::atomic<long> head_http{0};
inline std::atomic<int> head_curl{0};
inline std::atomic<bool> range_seen{false};
inline std::atomic<long> range_http{0};
inline std::atomic<int> range_curl{0};

inline void Reset() {
    file_errno.store(0);
    webdav_seen.store(false);
    head_http.store(0);
    head_curl.store(0);
    range_seen.store(false);
    range_http.store(0);
    range_curl.store(0);
}
} // namespace sphaira::open_diagnostics
