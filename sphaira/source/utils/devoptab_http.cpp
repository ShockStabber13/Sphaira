#include "utils/devoptab_common.hpp"
#include "utils/profile.hpp"

#include "location.hpp"
#include "log.hpp"
#include "defines.hpp"
#include "utils/open_diagnostics.hpp"
#include <sys/iosupport.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <minIni.h>

#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdio>
#include <strings.h>
#include <optional>
#include <unordered_map>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <sys/stat.h>

namespace sphaira::devoptab {
namespace {

struct DirEntry {
    // deprecated because the names can be truncated and really set to anything.
    std::string name_deprecated{};
    // url decoded href.
    std::string href{};
    bool is_dir{};
};
using DirEntries = std::vector<DirEntry>;

// Metadata-only range probe used when servers reject HEAD.
struct SizeProbe { curl_off_t total{-1}; };
size_t size_probe_header(char* ptr, size_t sz, size_t count, void* ctx) {
    const std::string_view line{ptr, sz * count};
    constexpr std::string_view key{"content-range:"};
    if (line.size() < key.size()) return sz * count;
    for (size_t i = 0; i < key.size(); ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != key[i]) return sz * count;
    }
    const auto slash = line.find('/');
    if (slash == std::string_view::npos) return sz * count;
    const std::string number{line.substr(slash + 1)};
    char* end = nullptr;
    const auto n = std::strtoull(number.c_str(), &end, 10);
    if (end != number.c_str() && n > 0 &&
        n <= static_cast<unsigned long long>(std::numeric_limits<curl_off_t>::max())) {
        static_cast<SizeProbe*>(ctx)->total = static_cast<curl_off_t>(n);
    }
    return sz * count;
}
size_t size_probe_stop_body(char*, size_t, size_t, void*) {
    return 0; // Stop at first body byte; do not download the file during stat.
}

// Store only the hostname of the final redirect destination.
void capture_final_hostname(CURL* handle, std::array<char, 96>& out) {
    out.fill(0);
    char* final_url = nullptr;
    curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &final_url);
    if (!final_url) return;

    CURLU* parts = curl_url();
    if (!parts) return;
    if (curl_url_set(parts, CURLUPART_URL, final_url, 0) == CURLUE_OK) {
        char* host = nullptr;
        if (curl_url_get(parts, CURLUPART_HOST, &host, 0) == CURLUE_OK && host) {
            std::snprintf(out.data(), out.size(), "%s", host);
        }
        curl_free(host);
    }
    curl_url_cleanup(parts);
}

struct FileEntry {
    std::string path{};
    struct stat st{};
    std::string url{};
};

struct File {
    FileEntry* entry;
    common::PushPullThreadData* push_pull_thread_data;
    size_t off;
    size_t last_off;
};

struct Dir {
    DirEntries* entries;
    size_t index;
};

struct Device final : common::MountCurlDevice {
    using MountCurlDevice::MountCurlDevice;

private:
    bool Mount() override;
    int devoptab_open(void *fileStruct, const char *path, int flags, int mode) override;
    int devoptab_close(void *fd) override;
    ssize_t devoptab_read(void *fd, char *ptr, size_t len) override;
    ssize_t devoptab_seek(void *fd, off_t pos, int dir) override;
    int devoptab_fstat(void *fd, struct stat *st) override;
    int devoptab_diropen(void* fd, const char *path) override;
    int devoptab_dirreset(void* fd) override;
    int devoptab_dirnext(void* fd, char *filename, struct stat *filestat) override;
    int devoptab_dirclose(void* fd) override;
    int devoptab_lstat(const char *path, struct stat *st) override;

    int http_dirlist(const std::string& path, DirEntries& out);
    int http_stat(const std::string& path, struct stat* st, bool is_dir);
    std::string resolve_link(const std::string& path, bool is_dir);

    // Maps synthetic file-browser paths to the exact HTTP links in directory listings.
    std::unordered_map<std::string, std::string> m_link_urls{};

private:
    bool mounted{};
};

int Device::http_dirlist(const std::string& path, DirEntries& out) {
    // A listed directory may point to a redirect or a query-based URL.
    // Reconstructing it from the displayed name drops that information.
    const auto url = resolve_link(path, true);
    std::vector<char> chunk;

    log_write("[HTTP] Listing URL: %s path: %s\n", url.c_str(), path.c_str());

    curl_set_common_options(this->curl, url);
    curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, (void *)&chunk);

    const auto res = curl_easy_perform(this->curl);
    if (res != CURLE_OK) {
        log_write("[HTTP] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return -EIO;
    }

    long response_code = 0;
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &response_code);

    // Relative hrefs in the received HTML are relative to the final page URL
    // after redirects, not necessarily the initial directory URL.
    const char* effective_url = nullptr;
    curl_easy_getinfo(this->curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::string listing_url = effective_url ? effective_url : url;

    switch (response_code) {
        case 200: // OK
        case 206: // Partial Content
            break;
        case 301: // Moved Permanently
        case 302: // Found
        case 303: // See Other
        case 307: // Temporary Redirect
        case 308: // Permanent Redirect
            return -EIO;
        case 401: // Unauthorized
        case 403: // Forbidden
            return -EACCES;
        case 404: // Not Found
            return -ENOENT;
        default:
            return -EIO;
    }

    log_write("[HTTP] Received %zu bytes for directory listing\n", chunk.size());

    SCOPED_TIMESTAMP("http_dirlist parse");

    // very fast/basic html parsing.
    // takes 17ms to parse 3MB html with 7641 entries.
    // todo: if i ever add an xml parser to sphaira, use that instead.
    // todo: for the above, benchmark the parser to ensure its faster than the my code.
    std::string_view chunk_view{chunk.data(), chunk.size()};

    const auto body_start = chunk_view.find("<body");
    const auto body_end = chunk_view.rfind("</body>");
    const auto table_start = chunk_view.find("<table");
    const auto table_end = chunk_view.rfind("</table>");

    std::string_view table_view{};

    // try and find the body, if this doesn't exist, fallback it's not a valid html page.
    if (body_start != std::string_view::npos && body_end != std::string_view::npos && body_end > body_start) {
        table_view = chunk_view.substr(body_start, body_end - body_start);
    }

    // try and find the table, massively speeds up parsing if it exists.
    // todo: this may cause issues with some web servers that don't use a table for listings.
    // todo: if table fails to fine anything, fallback to body_view.
    if (table_start != std::string_view::npos && table_end != std::string_view::npos && table_end > table_start) {
        table_view = chunk_view.substr(table_start, table_end - table_start);
    }

    if (!table_view.empty()) {
        const std::string_view href_tag_start = "<a href=\"";
        const std::string_view href_tag_end = "\">";
        const std::string_view anchor_tag_end = "</a>";

        size_t pos = 0;
        out.reserve(10000);

        for (;;) {
            const auto href_pos = table_view.find(href_tag_start, pos);
            if (href_pos == std::string_view::npos) {
                break; // no more href.
            }
            pos = href_pos + href_tag_start.length();

            const auto href_begin = pos;
            const auto href_end = table_view.find(href_tag_end, href_begin);
            if (href_end == std::string_view::npos) {
                break; // no more href.
            }

            const auto href_name_end = table_view.find('"', href_begin);
            if (href_name_end == std::string_view::npos || href_name_end < href_begin || href_name_end > href_end) {
                break; // invalid href.
            }

            const auto name_begin = href_end + href_tag_end.length();
            const auto name_end = table_view.find(anchor_tag_end, name_begin);
            if (name_end == std::string_view::npos) {
                break; // no more names.
            }

            pos = name_end + anchor_tag_end.length();
            // Keep href's original percent encoding/query; the visible browser name
            // remains decoded, but GET/HEAD must use the exact advertised link.
            const auto raw_href = html_decode(table_view.substr(href_begin, href_name_end - href_begin));
            auto href = url_decode(raw_href);
            auto name = url_decode(std::string{table_view.substr(name_begin, name_end - name_begin)});

            // skip empty names/links, root dir entry and links that are not actual files/dirs (e.g. sorting/filter controls).
            if (name.empty() || href.empty() || name == "/" || href.starts_with('?') || href.starts_with('#')) {
                continue;
            }

            // skip parent directory entry and external links.
            if (href == ".." || name == ".." || href.starts_with("../") || name.starts_with("../") || href.find("://") != std::string::npos) {
                continue;
            }

            const auto is_dir = href.ends_with('/');
            if (is_dir) {
                href.pop_back(); // remove the trailing '/'
            }

            // The browser treats href as an entry name and appends it to the
            // current directory path. Use that same synthetic path as a lookup key.
            std::string entry_path = path;
            if (entry_path.empty() || !entry_path.ends_with('/')) entry_path += '/';
            std::string_view relative = href;
            while (!relative.empty() && relative.front() == '/') relative.remove_prefix(1);
            entry_path += relative;

            // Resolve links relative to the actual listing URL, not the mount root.
            // Only accept same-origin links; do not send mount credentials off-site.
            if (!raw_href.starts_with("//")) {
                CURLU* link = curl_url();
                if (link) {
                    if (curl_url_set(link, CURLUPART_URL, listing_url.c_str(), 0) == CURLUE_OK &&
                        curl_url_set(link, CURLUPART_URL, raw_href.c_str(), 0) == CURLUE_OK) {
                        char *resolved{}, *original_host{}, *resolved_host{}, *original_scheme{}, *resolved_scheme{};
                        curl_url_get(link, CURLUPART_HOST, &resolved_host, 0);
                        curl_url_get(link, CURLUPART_SCHEME, &resolved_scheme, 0);
                        CURLU* base = curl_url();
                        if (base) {
                            if (curl_url_set(base, CURLUPART_URL, listing_url.c_str(), 0) == CURLUE_OK) {
                                curl_url_get(base, CURLUPART_HOST, &original_host, 0);
                                curl_url_get(base, CURLUPART_SCHEME, &original_scheme, 0);
                            }
                            curl_url_cleanup(base);
                        }
                        if (original_host && resolved_host && original_scheme && resolved_scheme &&
                            !strcasecmp(original_host, resolved_host) &&
                            !strcasecmp(original_scheme, resolved_scheme) &&
                            curl_url_get(link, CURLUPART_URL, &resolved, 0) == CURLUE_OK && resolved) {
                            m_link_urls.insert_or_assign(entry_path, resolved);
                        }
                        curl_free(resolved);
                        curl_free(original_host);
                        curl_free(resolved_host);
                        curl_free(original_scheme);
                        curl_free(resolved_scheme);
                    }
                    curl_url_cleanup(link);
                }
            }
            out.emplace_back(name, href, is_dir);
        }
    }

    log_write("[HTTP] Parsed %zu entries from directory listing\n", out.size());

    return 0;
}

std::string Device::resolve_link(const std::string& path, bool is_dir) {
    if (const auto it = m_link_urls.find(path); it != m_link_urls.end()) {
        return it->second;
    }
    return build_url(path, is_dir);
}

int Device::http_stat(const std::string& path, struct stat* st, bool is_dir) {
    std::memset(st, 0, sizeof(*st));
    const auto url = resolve_link(path, is_dir);

    auto& diag = sphaira::open_diagnostics::current;
    diag.driver = sphaira::open_diagnostics::Http;
    diag.stage = 5;
    diag.webdav_seen = true; // Shared display field indicates HTTP metadata was attempted.
    diag.range_seen = false;
    diag.head_http = diag.range_http = 0;
    diag.head_curl = diag.range_curl = 0;

    curl_set_common_options(this->curl, url);
    curl_easy_setopt(this->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(this->curl, CURLOPT_FILETIME, 1L);

    const auto head_rc = curl_easy_perform(this->curl);
    long response_code = 0;
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &response_code);
    diag.head_http = response_code;
    diag.head_curl = static_cast<int>(head_rc);
    curl_easy_getinfo(this->curl, CURLINFO_REDIRECT_COUNT, &diag.head_redirects);
    capture_final_hostname(this->curl, diag.final_host);

    curl_off_t file_size = -1;
    curl_easy_getinfo(this->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_size);
    curl_off_t file_time = 0;
    curl_easy_getinfo(this->curl, CURLINFO_FILETIME_T, &file_time);

    const char* content_type{};
    curl_easy_getinfo(this->curl, CURLINFO_CONTENT_TYPE, &content_type);
    const char* effective_url{};
    curl_easy_getinfo(this->curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    bool looks_like_dir = is_dir;
    if (effective_url && std::string_view{effective_url}.ends_with('/')) looks_like_dir = true;
    if (content_type && std::string_view{content_type}.starts_with("text/html")) looks_like_dir = true;

    const bool head_ok = head_rc == CURLE_OK && (response_code == 200 || response_code == 206);
    if (!is_dir && (!head_ok || (file_size <= 0 && !looks_like_dir))) {
        log_write("[HTTP] HEAD unusable (curl=%d http=%ld size=%lld), testing GET range\\n",
            static_cast<int>(head_rc), response_code, static_cast<long long>(file_size));

        SizeProbe probe{};
        curl_set_common_options(this->curl, url);
        curl_easy_setopt(this->curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(this->curl, CURLOPT_HEADERFUNCTION, size_probe_header);
        curl_easy_setopt(this->curl, CURLOPT_HEADERDATA, &probe);
        curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, size_probe_stop_body);
        diag.range_seen = true;
        const auto get_rc = curl_easy_perform(this->curl);
        long get_status = 0;
        curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &get_status);
        diag.range_http = get_status;
        diag.range_curl = static_cast<int>(get_rc);
        curl_easy_getinfo(this->curl, CURLINFO_REDIRECT_COUNT, &diag.range_redirects);
        capture_final_hostname(this->curl, diag.final_host);
        curl_off_t get_length = -1;
        curl_easy_getinfo(this->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &get_length);
        const curl_off_t total = get_status == 206 ? probe.total : get_status == 200 ? get_length : -1;

        // A deliberate callback abort is expected when the server sends file data.
        if ((get_rc == CURLE_OK || get_rc == CURLE_WRITE_ERROR) &&
            (get_status == 200 || get_status == 206) && total > 0) {
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            st->st_size = total;
            st->st_nlink = 1;
            return 0;
        }

        if (get_status == 401 || get_status == 403) return -EACCES;
        if (get_status == 404) return -ENOENT;
        return -EIO;
    }

    if (!head_ok) {
        if (response_code == 401 || response_code == 403) return -EACCES;
        if (response_code == 404) return -ENOENT;
        return -EIO;
    }

    if (looks_like_dir) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = file_size > 0 ? file_size : 0;
    }
    st->st_mtime = file_time > 0 ? file_time : 0;
    st->st_atime = st->st_mtime;
    st->st_ctime = st->st_mtime;
    st->st_nlink = 1;
    return 0;
}

bool Device::Mount() {
    if (mounted) {
        return true;
    }

    if (!MountCurlDevice::Mount()) {
        return false;
    }

    // todo: query server with OPTIONS to see if it supports range requests.
    // todo: see ftp for example.

    return mounted = true;
}

int Device::devoptab_open(void *fileStruct, const char *path, int flags, int mode) {
    auto file = static_cast<File*>(fileStruct);

    struct stat st;
    const auto ret = http_stat(path, &st, false);
    if (ret < 0) {
        log_write("[HTTP] http_stat() failed for file: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    if (st.st_mode & S_IFDIR) {
        log_write("[HTTP] Attempted to open a directory as a file: %s\n", path);
        return -EISDIR;
    }

    file->entry = new FileEntry{path, st, resolve_link(path, false)};
    return 0;
}

int Device::devoptab_close(void *fd) {
    auto file = static_cast<File*>(fd);

    delete file->push_pull_thread_data;
    delete file->entry;
    return 0;
}

ssize_t Device::devoptab_read(void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    len = std::min(len, file->entry->st.st_size - file->off);
    if (!len) return 0;

    if (file->off != file->last_off) {
        log_write("[HTTP] Seek from %zu to %zu: restarting stream\n",
            file->last_off, file->off);
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;
        file->last_off = file->off;
    }

    // Complete each read or return an error. The installer cannot interpret
    // a partial NCA read as success. Resume where the last stream stopped.
    constexpr unsigned max_reconnects = 5;
    constexpr s64 reconnect_delay_ns = 2'000'000'000LL;
    size_t total = 0;
    unsigned reconnects = 0;
    while (total < len) {
        if (!file->push_pull_thread_data) {
            log_write("[HTTP] Starting stream at %zu (remaining %zu)\n",
                file->off, len - total);
            file->push_pull_thread_data =
                CreatePushData(this->transfer_curl, file->entry->url, file->off);
            if (!file->push_pull_thread_data) {
                log_write("[HTTP] Failed to start stream at %zu\n", file->off);
                return -EIO;
            }
        }

        auto* transfer = file->push_pull_thread_data;
        const size_t n = transfer->PullData(ptr + total, len - total);
        total += n;
        file->off += n;
        file->last_off = file->off;
        if (total == len) return total;

        // The worker has finished (or failed): inspect its final result.
        const long http_status = transfer->code;
        const CURLcode curl_rc = transfer->curl_result;
        const bool rejected = transfer->rejected_response;
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;

        if (http_status == 401 || http_status == 403) {
            log_write("[HTTP] Authentication failed or link expired: HTTP %ld\n", http_status);
            return -EACCES;
        }
        if (http_status == 404) return -ENOENT;
        if (rejected || (http_status != 0 && http_status != 200 && http_status != 206)) {
            log_write("[HTTP] Invalid HTTP response: %ld, CURL %d, rejected %d\n",
                http_status, static_cast<int>(curl_rc), static_cast<int>(rejected));
            return -EIO;
        }

        const bool recoverable = curl_rc == CURLE_OK ||
            curl_rc == CURLE_PARTIAL_FILE ||
            curl_rc == CURLE_RECV_ERROR ||
            curl_rc == CURLE_OPERATION_TIMEDOUT ||
            curl_rc == CURLE_COULDNT_CONNECT ||
            curl_rc == CURLE_SEND_ERROR ||
            curl_rc == CURLE_GOT_NOTHING ||
            curl_rc == CURLE_SSL_CONNECT_ERROR;
        if (!recoverable || reconnects >= max_reconnects) {
            log_write("[HTTP] Stream failed after %u retries at %zu: CURL %d, HTTP %ld\n",
                reconnects, file->off, static_cast<int>(curl_rc), http_status);
            return -EIO;
        }
        ++reconnects;
        log_write("[HTTP] Reconnect at byte %zu, attempt %u/%u (CURL %d, HTTP %ld)\n",
            file->off, reconnects, max_reconnects,
            static_cast<int>(curl_rc), http_status);
        svcSleepThread(reconnect_delay_ns);
    }
    return total;
}

ssize_t Device::devoptab_seek(void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = file->entry->st.st_size;
    }

    return file->off = std::clamp<u64>(pos, 0, file->entry->st.st_size);
}

int Device::devoptab_fstat(void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);

    std::memcpy(st, &file->entry->st, sizeof(*st));
    return 0;
}

int Device::devoptab_diropen(void* fd, const char *path) {
    auto dir = static_cast<Dir*>(fd);

    log_write("[HTTP] Opening directory: %s\n", path);
    auto entries = new DirEntries();
    const auto ret = http_dirlist(path, *entries);
    if (ret < 0) {
        log_write("[HTTP] http_dirlist() failed for directory: %s errno: %s\n", path, std::strerror(-ret));
        delete entries;
        return ret;
    }

    log_write("[HTTP] Opened directory: %s with %zu entries\n", path, entries->size());
    dir->entries = entries;
    return 0;
}

int Device::devoptab_dirreset(void* fd) {
    auto dir = static_cast<Dir*>(fd);

    dir->index = 0;
    return 0;
}

int Device::devoptab_dirnext(void* fd, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(fd);

    if (dir->index >= dir->entries->size()) {
        return -ENOENT;
    }

    auto& entry = (*dir->entries)[dir->index];
    if (entry.is_dir) {
        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
    } else {
        filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    }

    // <a href="Compass_2.0.7.1-Release_ScVi3.0.1-Standalone-21-2-0-7-1-1729820977.zip">Compass_2.0.7.1-Release_ScVi3.0.1-Standalone-21..&gt;</a>
    filestat->st_nlink = 1;
    // std::strcpy(filename, entry.name.c_str());
    std::strcpy(filename, entry.href.c_str());

    dir->index++;
    return 0;
}

int Device::devoptab_dirclose(void* fd) {
    auto dir = static_cast<Dir*>(fd);

    delete dir->entries;
    return 0;
}

int Device::devoptab_lstat(const char *path, struct stat *st) {
    auto ret = http_stat(path, st, false);
    if (ret < 0) {
        ret = http_stat(path, st, true);
    }

    if (ret < 0) {
        log_write("[HTTP] http_stat() failed for path: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    return 0;
}

} // namespace

Result MountHttpAll() {
    return common::MountNetworkDevice([](const common::MountConfig& config) {
            return std::make_unique<Device>(config);
        },
        sizeof(File), sizeof(Dir),
        "HTTP",
        true
    );
}

} // namespace sphaira::devoptab
