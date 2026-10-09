#pragma once

// One probe per thread, frozen only if that thread's fopen() fails.
// No filenames, URLs or credentials are recorded.
#include <atomic>

namespace sphaira::open_diagnostics {

enum Driver : int { Unknown = 0, WebDav = 1, Mounts = 2 };

struct Attempt {
    int stage{};
    int path_length{};
    int mount_length{-1};
    int driver{};
    int flags{};
    bool webdav_seen{};
    bool range_seen{};
    long head_http{};
    int head_curl{};
    long range_http{};
    int range_curl{};
};

inline thread_local Attempt current{};
inline std::atomic<int> file_errno{};
inline std::atomic<int> open_stage{};
inline std::atomic<int> path_length{};
inline std::atomic<int> mount_length{-1};
inline std::atomic<int> driver{};
inline std::atomic<int> open_flags{};
inline std::atomic<bool> webdav_seen{};
inline std::atomic<long> head_http{};
inline std::atomic<int> head_curl{};
inline std::atomic<bool> range_seen{};
inline std::atomic<long> range_http{};
inline std::atomic<int> range_curl{};

inline void Reset() {
    current = {};
}

inline void CaptureFailure(int error) {
    file_errno.store(error);
    open_stage.store(current.stage);
    path_length.store(current.path_length);
    mount_length.store(current.mount_length);
    driver.store(current.driver);
    open_flags.store(current.flags);
    webdav_seen.store(current.webdav_seen);
    head_http.store(current.head_http);
    head_curl.store(current.head_curl);
    range_seen.store(current.range_seen);
    range_http.store(current.range_http);
    range_curl.store(current.range_curl);
}
} // namespace sphaira::open_diagnostics
