#pragma once

// Debug-only information displayed when opening a remote file fails.
// Do not store credentials, URLs, or filenames here.
#include <atomic>

namespace sphaira::open_diagnostics {
// File-open dispatch stages: 0=not entered, 1=callback, 2=path fixed,
// 3=mounted, 4=driver called, 5=WebDAV stat, 6=driver succeeded.
inline std::atomic<int> open_stage{0};
inline std::atomic<int> path_length{0};
inline std::atomic<int> mount_length{-1};
inline std::atomic<int> file_errno{0};
inline std::atomic<bool> webdav_seen{false};
inline std::atomic<long> head_http{0};
inline std::atomic<int> head_curl{0};
inline std::atomic<bool> range_seen{false};
inline std::atomic<long> range_http{0};
inline std::atomic<int> range_curl{0};

inline void Reset() {
    open_stage.store(0);
    path_length.store(0);
    mount_length.store(-1);
    file_errno.store(0);
    webdav_seen.store(false);
    head_http.store(0);
    head_curl.store(0);
    range_seen.store(false);
    range_http.store(0);
    range_curl.store(0);
}
} // namespace sphaira::open_diagnostics
