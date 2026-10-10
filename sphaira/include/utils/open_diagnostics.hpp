#pragma once

// One probe per thread, frozen only if that thread's fopen() fails.
// No filenames, URLs or credentials are recorded.
#include <atomic>
#include <array>
#include <cstdint>
#include <string>

namespace sphaira::open_diagnostics {

enum Driver : int { Unknown = 0, WebDav = 1, Mounts = 2, Http = 3 };

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
    long head_redirects{};
    long range_redirects{};
    std::array<char, 96> final_host{};
};

inline thread_local Attempt current{};
// First 16 bytes only, recorded only on a failed NSP magic check.
// Never record URLs, paths, passwords, or entire response bodies.
inline std::array<std::atomic<unsigned char>, 16> invalid_nsp_header{};
inline std::atomic<bool> invalid_nsp_header_valid{false};
// Capture only generic HTTP transfer metadata, never file URLs or tokens.
inline std::atomic<long> download_status{0};
inline std::atomic<long> download_redirect_count{0};
inline std::atomic<int> download_mime_kind{0}; // 1 XML, 2 HTML, 3 octet, 4 other
inline std::atomic<bool> download_seen{false};
// Streaming failure diagnostics: status alone cannot establish a successful read.
inline std::atomic<int> download_curl_result{-1}; // -1 = still running / not started
inline std::atomic<bool> download_range_valid{false};
inline std::atomic<bool> download_rejected{false};
inline std::atomic<std::uint64_t> download_payload_bytes{0};
inline std::atomic<std::uint64_t> webdav_file_size{0};
inline std::atomic<std::uint64_t> webdav_read_requested{0};
inline std::atomic<std::uint64_t> webdav_read_allowed{0};

// Metadata-only NSP parser diagnostics, captured without paths or payload.
// The stage is a numbered validation or read operation (see nsp.cpp).
inline std::atomic<bool> nsp_diag_valid{false};
inline std::atomic<int> nsp_stage{0};
inline std::atomic<std::uint64_t> nsp_files{0};
inline std::atomic<std::uint64_t> nsp_strings{0};
inline std::atomic<std::uint64_t> nsp_file_index{0};
inline std::atomic<std::uint64_t> nsp_read_offset{0};
inline std::atomic<std::uint64_t> nsp_read_expected{0};
inline std::atomic<std::uint64_t> nsp_read_received{0};
inline std::atomic<bool> nsp_read_short{false};

inline void CaptureInvalidNspHeader(const void* data, std::size_t len) {
    invalid_nsp_header_valid.store(false);
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < invalid_nsp_header.size(); ++i)
        invalid_nsp_header[i].store(i < len ? bytes[i] : 0);
    invalid_nsp_header_valid.store(true);
}

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
inline std::atomic<long> head_redirects{};
inline std::atomic<long> range_redirects{};
inline std::array<std::atomic<char>, 96> final_host{};

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
    head_redirects.store(current.head_redirects);
    range_redirects.store(current.range_redirects);
    for (size_t i = 0; i < final_host.size(); ++i) {
        final_host[i].store(current.final_host[i]);
    }
}
inline std::string GetFinalHost() {
    std::string result;
    result.reserve(final_host.size());
    for (const auto& item : final_host) {
        const auto c = item.load();
        if (!c) break;
        result += c;
    }
    return result;
}
} // namespace sphaira::open_diagnostics
