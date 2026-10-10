#include "utils/devoptab_common.hpp"
#include "utils/profile.hpp"

#include "log.hpp"
#include "defines.hpp"
#include "utils/open_diagnostics.hpp"
#include <fcntl.h>
#include <curl/curl.h>

#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <optional>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <sys/stat.h>

// todo: try to reduce binary size by using a smaller xml parser.
#include <pugixml.hpp>

namespace sphaira::devoptab {
namespace {

constexpr const char* XPATH_RESPONSE      = "//*[local-name()='response']";
constexpr const char* XPATH_HREF          = ".//*[local-name()='href']";
constexpr const char* XPATH_PROPSTAT_PROP = ".//*[local-name()='propstat']/*[local-name()='prop']";
constexpr const char* XPATH_PROP          = ".//*[local-name()='prop']";
constexpr const char* XPATH_RESOURCETYPE  = ".//*[local-name()='resourcetype']";
constexpr const char* XPATH_COLLECTION    = ".//*[local-name()='collection']";

struct DirEntry {
    std::string name{};
    bool is_dir{};
};
using DirEntries = std::vector<DirEntry>;

struct FileEntry {
    std::string path{};
    struct stat st{};
};

// TorBox may reject HEAD but support HTTP byte-range GET.
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
    return 0; // Metadata only; never download the game during stat().
}

struct File {
    FileEntry* entry;
    common::PushPullThreadData* push_pull_thread_data;
    size_t off;
    size_t last_off;
    size_t download_window_end; // exclusive end of active bounded HTTP request
    size_t download_window_size; // dynamically backs off when TorBox rejects a large range
    unsigned good_window_count;
    bool write_mode;
};

struct Dir {
    DirEntries* entries;
    size_t index;
};

struct Device final : common::MountCurlDevice {
    using MountCurlDevice::MountCurlDevice;

private:
    int devoptab_open(void *fileStruct, const char *path, int flags, int mode) override;
    int devoptab_close(void *fd) override;
    ssize_t devoptab_read(void *fd, char *ptr, size_t len) override;
    ssize_t devoptab_write(void *fd, const char *ptr, size_t len) override;
    ssize_t devoptab_seek(void *fd, off_t pos, int dir) override;
    int devoptab_fstat(void *fd, struct stat *st) override;
    int devoptab_unlink(const char *path) override;
    int devoptab_rename(const char *oldName, const char *newName) override;
    int devoptab_mkdir(const char *path, int mode) override;
    int devoptab_rmdir(const char *path) override;
    int devoptab_diropen(void* fd, const char *path) override;
    int devoptab_dirreset(void* fd) override;
    int devoptab_dirnext(void* fd, char *filename, struct stat *filestat) override;
    int devoptab_dirclose(void* fd) override;
    int devoptab_lstat(const char *path, struct stat *st) override;
    int devoptab_ftruncate(void *fd, off_t len) override;
    int devoptab_fsync(void *fd) override;

    std::pair<bool, long> webdav_custom_command(const std::string& path, const std::string& cmd, std::string_view postfields, std::span<const std::string> headers, bool is_dir, std::vector<char>* response_data = nullptr);
    int webdav_dirlist(const std::string& path, DirEntries& out);
    int webdav_stat(const std::string& path, struct stat* st, bool is_dir);
    int webdav_remove_file_folder(const std::string& path, bool is_dir);
    int webdav_unlink(const std::string& path);
    int webdav_rename(const std::string& old_path, const std::string& new_path, bool is_dir);
    int webdav_mkdir(const std::string& path);
    int webdav_rmdir(const std::string& path);
};

size_t dummy_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

std::pair<bool, long> Device::webdav_custom_command(const std::string& path, const std::string& cmd, std::string_view postfields, std::span<const std::string> headers, bool is_dir, std::vector<char>* response_data) {
    const auto url = build_url(path, is_dir);

    curl_slist* header_list{};
    ON_SCOPE_EXIT(curl_slist_free_all(header_list));

    for (const auto& header : headers) {
        log_write("[WEBDAV] Header: %s\n", header.c_str());
        header_list = curl_slist_append(header_list, header.c_str());
    }

    // The full URL contains the WebDAV API key; never write it to log.txt.\n    log_write("[WEBDAV] %s request (path length=%zu)\n", cmd.c_str(), path.size());
    curl_set_common_options(this->curl, url);
    curl_easy_setopt(this->curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(this->curl, CURLOPT_CUSTOMREQUEST, cmd.c_str());
    if (!postfields.empty()) {
        log_write("[WEBDAV] Post fields: %.*s\n", (int)postfields.length(), postfields.data());
        curl_easy_setopt(this->curl, CURLOPT_POSTFIELDS, postfields.data());
        curl_easy_setopt(this->curl, CURLOPT_POSTFIELDSIZE, (long)postfields.length());
    }

    if (response_data) {
        response_data->clear();
        curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
        curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, (void *)response_data);
    } else {
        curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, dummy_data_callback);
    }

    const auto res = curl_easy_perform(this->curl);
    if (res != CURLE_OK) {
        log_write("[WEBDAV] curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return {false, 0};
    }

    long response_code = 0;
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &response_code);
    return {true, response_code};
}

int Device::webdav_dirlist(const std::string& path, DirEntries& out) {
    const std::string_view post_fields =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
        "<d:propfind xmlns:d=\"DAV:\">"
            "<d:prop>"
            // "<d:getcontentlength/>"
            "<d:resourcetype/>"
        "</d:prop>"
        "</d:propfind>";

    const std::string custom_headers[] = {
        "Content-Type: application/xml; charset=utf-8",
        "Depth: 1"
    };

    std::vector<char> chunk;
    const auto [success, response_code] = webdav_custom_command(path, "PROPFIND", post_fields, custom_headers, true, &chunk);
    if (!success) {
        return -EIO;
    }

    switch (response_code) {
        case 207: // Multi-Status
            break;
        case 404: // Not Found
            return -ENOENT;
        case 403: // Forbidden
            return -EACCES;
        default:
            log_write("[WEBDAV] Unexpected HTTP response code: %ld\n", response_code);
            return -EIO;
    }

    SCOPED_TIMESTAMP("webdav_dirlist parse");

    pugi::xml_document doc;
    const auto result = doc.load_buffer_inplace(chunk.data(), chunk.size());
    if (!result) {
        log_write("[WEBDAV] Failed to parse XML: %s\n", result.description());
        return -EIO;
    }

    log_write("\n[WEBDAV] XML parsed successfully\n");

    auto requested_path = url_decode(path);
    if (!requested_path.empty() && requested_path.back() == '/') {
        requested_path.pop_back();
    }

    const auto responses = doc.select_nodes(XPATH_RESPONSE);

    for (const auto& rnode : responses) {
        const auto response = rnode.node();
        if (!response) {
            continue;
        }

        const auto href_x = response.select_node(XPATH_HREF);
        if (!href_x) {
            continue;
        }

        // todo: fix requested path still being displayed.
        const auto href = url_decode(href_x.node().text().as_string());
        if (href.empty() || href == requested_path || href == requested_path + '/') {
            continue;
        }

        // propstat/prop/resourcetype
        auto prop_x = response.select_node(XPATH_PROPSTAT_PROP);
        if (!prop_x) {
            // try direct prop if structure differs
            prop_x = response.select_node(XPATH_PROP);
            if (!prop_x) {
                continue;
            }
        }

        const auto prop = prop_x.node();
        const auto rtype_x = prop.select_node(XPATH_RESOURCETYPE);
        bool is_dir = false;
        if (rtype_x && rtype_x.node().select_node(XPATH_COLLECTION)) {
            is_dir = true;
        }

        auto name = href;
        if (!name.empty() && name.back() == '/') {
            name.pop_back();
        }

        const auto pos = name.find_last_of('/');
        if (pos != std::string::npos) {
            name = name.substr(pos + 1);
        }

        // skip root entry
        if (name.empty() || name == ".") {
            continue;
        }

        out.emplace_back(name, is_dir);
    }

    log_write("[WEBDAV] Parsed %zu entries from directory listing\n", out.size());

    return 0;
}

// todo: use PROPFIND to get file size and time, although it is slower...
int Device::webdav_stat(const std::string& path, struct stat* st, bool is_dir) {
    std::memset(st, 0, sizeof(*st));
    const auto url = build_url(path, is_dir);

    curl_set_common_options(this->curl, url);
    curl_easy_setopt(this->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(this->curl, CURLOPT_FILETIME, 1L);
    sphaira::open_diagnostics::current.stage = 5;
    sphaira::open_diagnostics::current.driver = sphaira::open_diagnostics::WebDav;
    sphaira::open_diagnostics::current.webdav_seen = true;
    const auto head_rc = curl_easy_perform(this->curl);

    long status = 0;
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t length = -1;
    curl_easy_getinfo(this->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    curl_off_t file_time = 0;
    curl_easy_getinfo(this->curl, CURLINFO_FILETIME_T, &file_time);

    sphaira::open_diagnostics::current.head_http = status;
    sphaira::open_diagnostics::current.head_curl = static_cast<int>(head_rc);
    const bool head_ok = head_rc == CURLE_OK && (status == 200 || status == 206);
    if (!is_dir && (!head_ok || length <= 0)) {
        // TorBox's WebDAV can return 207 to HEAD and a chunked 200 to GET
        // (no Content-Length). Its PROPFIND endpoint provides the actual
        // file metadata. Query that before attempting the Range fallback.
        constexpr std::string_view props =
            "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
            "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
            "<d:getcontentlength/><d:resourcetype/>"
            "</d:prop></d:propfind>";
        const std::string prop_headers[] = {
            "Content-Type: application/xml; charset=utf-8",
            "Depth: 0",
        };
        std::vector<char> prop_xml;
        const auto [prop_ok, prop_http] = webdav_custom_command(
            path, "PROPFIND", props, prop_headers, false, &prop_xml);
        if (prop_ok && prop_http == 207 && !prop_xml.empty()) {
            pugi::xml_document doc;
            if (doc.load_buffer(prop_xml.data(), prop_xml.size())) {
                const auto responses = doc.select_nodes(XPATH_RESPONSE);
                // Depth: 0 must describe exactly the requested resource.
                // Never mistake a directory entry for a downloadable file.
                if (responses.size() == 1 &&
                    !responses[0].node().select_node(
                        ".//*[local-name()='collection']")) {
                    const auto stats = responses[0].node().select_nodes(
                        ".//*[local-name()='propstat']");
                    for (const auto& entry : stats) {
                        const auto http_status = entry.node().select_node(
                            "./*[local-name()='status']");
                        const std::string_view status_text =
                            http_status ? http_status.node().text().as_string() : "";
                        if (status_text.find(" 200 ") == std::string_view::npos)
                            continue;
                        const auto size_node = entry.node().select_node(
                            "./*[local-name()='prop']/*[local-name()='getcontentlength']");
                        if (!size_node)
                            continue;
                        const char* raw_size = size_node.node().text().as_string();
                        if (!raw_size || !*raw_size)
                            continue;
                        char* end = nullptr;
                        const auto parsed = std::strtoull(raw_size, &end, 10);
                        if (end == raw_size || *end != '\0' || parsed == 0 ||
                            parsed > static_cast<unsigned long long>(
                                std::numeric_limits<curl_off_t>::max()))
                            continue;
                        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
                        st->st_size = static_cast<curl_off_t>(parsed);
                        st->st_nlink = 1;
                        log_write("[WEBDAV] PROPFIND recovered file size (%llu bytes)\\n",
                            parsed);
                        return 0;
                    }
                }
            }
        }

        log_write("[WEBDAV] HEAD unusable (curl=%d http=%ld size=%lld); probing range\\n",
            static_cast<int>(head_rc), status, static_cast<long long>(length));

        SizeProbe probe{};
        curl_set_common_options(this->curl, url);
        curl_easy_setopt(this->curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(this->curl, CURLOPT_HEADERFUNCTION, size_probe_header);
        curl_easy_setopt(this->curl, CURLOPT_HEADERDATA, &probe);
        curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, size_probe_stop_body);
        curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, nullptr);
        sphaira::open_diagnostics::current.range_seen = true;
        const auto get_rc = curl_easy_perform(this->curl);
        curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &status);
        sphaira::open_diagnostics::current.range_http = status;
        sphaira::open_diagnostics::current.range_curl = static_cast<int>(get_rc);
        curl_off_t response_length = -1;
        curl_easy_getinfo(this->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &response_length);
        length = status == 206 ? probe.total : status == 200 ? response_length : -1;

        if ((get_rc == CURLE_OK || get_rc == CURLE_WRITE_ERROR) && length > 0 &&
            (status == 206 || status == 200)) {
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            st->st_size = length;
            st->st_nlink = 1;
            log_write("[WEBDAV] Range probe determined size: %lld\\n",
                static_cast<long long>(length));
            return 0;
        }

        log_write("[WEBDAV] Range probe failed (curl=%d http=%ld size=%lld)\\n",
            static_cast<int>(get_rc), status, static_cast<long long>(length));
        if (status == 401 || status == 403) return -EACCES;
        if (status == 404) return -ENOENT;
        return -EIO;
    }

    if (!head_ok) {
        log_write("[WEBDAV] HEAD failed (curl=%d http=%ld)\\n",
            static_cast<int>(head_rc), status);
        if (status == 401 || status == 403) return -EACCES;
        if (status == 404) return -ENOENT;
        return -EIO;
    }

    st->st_mode = (is_dir ? S_IFDIR : S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_size = is_dir ? 0 : length;
    st->st_mtime = file_time > 0 ? file_time : 0;
    st->st_atime = st->st_mtime;
    st->st_ctime = st->st_mtime;
    st->st_nlink = 1;
    return 0;
}

int Device::webdav_remove_file_folder(const std::string& path, bool is_dir) {
    const auto [success, response_code] = webdav_custom_command(path, "DELETE", "", {}, is_dir);
    if (!success) {
        return -EIO;
    }

    switch (response_code) {
        case 200: // OK
        case 204: // No Content
            return 0;
        case 404: // Not Found
            return -ENOENT;
        case 403: // Forbidden
            return -EACCES;
        case 409: // Conflict
            return -ENOTEMPTY; // Directory not empty
        default:
            return -EIO;
    }
}

int Device::webdav_unlink(const std::string& path) {
    return webdav_remove_file_folder(path, false);
}

int Device::webdav_rename(const std::string& old_path, const std::string& new_path, bool is_dir) {
    log_write("[WEBDAV] Renaming %s to %s\n", old_path.c_str(), new_path.c_str());

    const std::string custom_headers[] = {
        "Destination: " + build_url(new_path, is_dir),
        "Overwrite: T",
    };

    const auto [success, response_code] = webdav_custom_command(old_path, "MOVE", "", custom_headers, is_dir);

    if (!success) {
        return -EIO;
    }

    switch (response_code) {
        case 201: // Created
        case 204: // No Content
            return 0;
        case 404: // Not Found
            return -ENOENT;
        case 403: // Forbidden
            return -EACCES;
        case 412: // Precondition Failed
            return -EEXIST; // Destination already exists and Overwrite is F
        case 409: // Conflict
            return -ENOENT; // Parent directory of destination does not exist
        default:
            return -EIO;
    }
}

int Device::webdav_mkdir(const std::string& path) {
    const auto [success, response_code] = webdav_custom_command(path, "MKCOL", "", {}, true);
    if (!success) {
        return -EIO;
    }

    switch (response_code) {
        case 201: // Created
            return 0;
        case 405: // Method Not Allowed
            return -EEXIST; // Collection already exists
        case 409: // Conflict
            return -ENOENT; // Parent collection does not exist
        case 403: // Forbidden
            return -EACCES;
        default:
            return -EIO;
    }
}

int Device::webdav_rmdir(const std::string& path) {
    return webdav_remove_file_folder(path, true);
}

int Device::devoptab_open(void *fileStruct, const char *path, int flags, int mode) {
    auto file = static_cast<File*>(fileStruct);
    struct stat st{};

    // append mode is not supported.
    if (flags & O_APPEND) {
        return -E2BIG;
    }

    if ((flags & O_ACCMODE) == O_RDONLY) {
        // ensure the file exists and get its size.
        const auto ret = webdav_stat(path, &st, false);
        if (ret < 0) {
            return ret;
        }

        if (st.st_mode & S_IFDIR) {
            log_write("[WEBDAV] Path is a directory, not a file: %s\n", path);
            return -EISDIR;
        }
    }

    sphaira::open_diagnostics::webdav_file_size.store(
        st.st_size > 0 ? static_cast<u64>(st.st_size) : 0);
    sphaira::open_diagnostics::webdav_read_failed.store(false);
    log_write("[WEBDAV] Opening file: %s\n", path);
    file->entry = new FileEntry{path, st};
    file->write_mode = (flags & (O_WRONLY | O_RDWR));

    return 0;
}

int Device::devoptab_close(void *fd) {
    auto file = static_cast<File*>(fd);

    log_write("[WEBDAV] Closing file: %s\n", file->entry->path.c_str());
    delete file->push_pull_thread_data;
    delete file->entry;
    return 0;
}

ssize_t Device::devoptab_read(void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    if (file->write_mode) {
        return -EBADF;
    }

    // Unlike unbounded WebDAV GET, TorBox's bounded Range requests return
    // the raw binary and an accurate Content-Range. Download 4 MiB windows,
    // filling a read even when it crosses one or more window boundaries.
    constexpr size_t max_window_size = 4 * 1024 * 1024;
    constexpr size_t min_window_size = 4 * 1024;
    constexpr unsigned max_reconnects = 12;
    constexpr s64 reconnect_delay_ns = 500'000'000LL;
    if (!file->download_window_size)
        file->download_window_size = max_window_size;
    const size_t file_size =
        static_cast<size_t>(std::max<off_t>(0, file->entry->st.st_size));
    sphaira::open_diagnostics::webdav_read_requested.store(len);
    if (file->off >= file_size) {
        sphaira::open_diagnostics::webdav_read_allowed.store(0);
        return 0;
    }
    len = std::min(len, file_size - file->off);
    sphaira::open_diagnostics::webdav_read_allowed.store(len);
    if (!len) return 0;

    // A random seek cannot reuse the old stream's buffered bytes.
    if (file->off != file->last_off) {
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;
        file->download_window_end = 0;
        file->last_off = file->off;
    }

    auto report_failure = [&](int reason, long http, CURLcode curl_code, bool rejected, unsigned tries) {
        namespace diag = sphaira::open_diagnostics;
        diag::webdav_read_failed.store(true);
        diag::webdav_failure_reason.store(reason);
        diag::webdav_failure_offset.store(file->off);
        diag::webdav_failure_end.store(file->download_window_end);
        diag::webdav_failure_window.store(file->download_window_size);
        diag::webdav_failure_http.store(http);
        diag::webdav_failure_curl.store(static_cast<int>(curl_code));
        diag::webdav_failure_rejected.store(rejected);
        diag::webdav_failure_attempts.store(static_cast<int>(tries));
    };
    size_t total = 0;
    unsigned reconnects = 0;
    while (total < len) {
        // The previous bounded window has been fully consumed.
        if (file->push_pull_thread_data &&
            file->off >= file->download_window_end) {
            delete file->push_pull_thread_data;
            file->push_pull_thread_data = nullptr;
            // After consistently successful windows, cautiously increase
            // throughput while retaining the ability to back off again.
            if (++file->good_window_count >= 16 &&
                file->download_window_size < max_window_size) {
                file->good_window_count = 0;
                file->download_window_size = std::min(
                    max_window_size, file->download_window_size * 2);
            }
        }

        if (!file->push_pull_thread_data) {
            file->download_window_end = file->off +
                std::min(file->download_window_size, file_size - file->off);
            log_write("[WEBDAV] Bounded range %zu-%zu\n",
                file->off, file->download_window_end - 1);
            file->push_pull_thread_data = CreatePushData(
                this->transfer_curl,
                build_url(file->entry->path, false),
                file->off, true, file->download_window_end);
            if (!file->push_pull_thread_data) {
                report_failure(1, 0, CURLE_FAILED_INIT, false, reconnects);
                return -EIO;
            }
        }

        // Never consume bytes beyond the requested window: the following
        // bytes must be fetched by a fresh range request.
        const size_t wanted = std::min(
            len - total, file->download_window_end - file->off);
        auto* transfer = file->push_pull_thread_data;
        const size_t read_now = transfer->PullData(ptr + total, wanted);
        total += read_now;
        file->off += read_now;
        file->last_off = file->off;

        if (read_now == wanted) {
            reconnects = 0;
            continue;
        }

        // The stream ended before supplying the complete window.
        // An HTTP 200 (HTML viewer) or malformed Content-Range is never
        // treated as a successful file read.
        const long http_status = transfer->code;
        const CURLcode curl_rc = transfer->curl_result;
        const bool rejected = transfer->rejected_response;
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;
        // Keep the last attempted end for failure diagnostics; a new
        // stream will replace this value on the next retry.

        if (http_status == 401 || http_status == 403) {
            report_failure(2, http_status, curl_rc, rejected, reconnects);
            return -EACCES;
        }
        if (http_status == 404) {
            report_failure(3, http_status, curl_rc, rejected, reconnects);
            return -ENOENT;
        }

        // A 200/HTML response often means TorBox disregarded this range.
        // Before giving up, retry the same unread bytes with a smaller,
        // explicitly bounded range. Never accept HTTP 200 as NSP bytes.
        const bool recoverable_curl = curl_rc == CURLE_OK ||
            curl_rc == CURLE_PARTIAL_FILE ||
            curl_rc == CURLE_RECV_ERROR ||
            curl_rc == CURLE_OPERATION_TIMEDOUT ||
            curl_rc == CURLE_COULDNT_CONNECT ||
            curl_rc == CURLE_SEND_ERROR ||
            curl_rc == CURLE_GOT_NOTHING ||
            curl_rc == CURLE_SSL_CONNECT_ERROR;
        const bool server_rejected_range =
            rejected && (http_status == 200 || http_status == 206);
        const bool retryable_status = http_status == 0 ||
            http_status == 200 || http_status == 206;
        if (!retryable_status ||
            !(server_rejected_range || recoverable_curl) ||
            reconnects >= max_reconnects) {
            report_failure(rejected || !retryable_status ? 3 : 4,
                           http_status, curl_rc, rejected, reconnects);
            log_write("[WEBDAV] Unable to read at %zu: HTTP %ld, CURL %d, rejected %d, tries %u\n",
                file->off, http_status, static_cast<int>(curl_rc),
                static_cast<int>(rejected), reconnects);
            return -EIO;
        }

        if (file->download_window_size > min_window_size) {
            file->download_window_size = std::max(
                min_window_size, file->download_window_size / 2);
        }
        file->good_window_count = 0;
        ++reconnects;
        log_write("[WEBDAV] Retry at %zu with %zu-byte range (%u/%u, HTTP %ld, CURL %d)\n",
            file->off, file->download_window_size, reconnects,
            max_reconnects, http_status, static_cast<int>(curl_rc));
        svcSleepThread(reconnect_delay_ns);
    }

    return total;
}

ssize_t Device::devoptab_write(void *fd, const char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);

    if (!file->write_mode) {
        log_write("[WEBDAV] Attempt to write to a read-only file\n");
        return -EBADF;
    }

    if (!len) {
        return 0;
    }

    if (!file->push_pull_thread_data) {
        log_write("[WEBDAV] Creating upload thread data for file: %s\n", file->entry->path.c_str());
        file->push_pull_thread_data = CreatePullData(this->transfer_curl, build_url(file->entry->path, false));
        if (!file->push_pull_thread_data) {
            log_write("[WEBDAV] Failed to create upload thread data for file: %s\n", file->entry->path.c_str());
            return -EIO;
        }
    }

    const auto ret = file->push_pull_thread_data->PushData(ptr, len);

    file->off += ret;
    file->entry->st.st_size = std::max<off_t>(file->entry->st.st_size, file->off);
    return ret;
}

ssize_t Device::devoptab_seek(void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = file->entry->st.st_size;
    }

    // for now, random access writes are disabled.
    if (file->write_mode && pos != file->off) {
        log_write("[WEBDAV] Random access writes are not supported\n");
        return file->off;
    }

    return file->off = std::clamp<u64>(pos, 0, file->entry->st.st_size);
}

int Device::devoptab_fstat(void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);

    std::memcpy(st, &file->entry->st, sizeof(*st));
    return 0;
}

int Device::devoptab_unlink(const char *path) {
    const auto ret = webdav_unlink(path);
    if (ret < 0) {
        log_write("[WEBDAV] webdav_unlink() failed: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    return 0;
}

int Device::devoptab_rename(const char *oldName, const char *newName) {
    auto ret = webdav_rename(oldName, newName, false);
    if (ret == -ENOENT) {
        ret = webdav_rename(oldName, newName, true);
    }

    if (ret < 0) {
        log_write("[WEBDAV] webdav_rename() failed: %s to %s errno: %s\n", oldName, newName, std::strerror(-ret));
        return ret;
    }

    return 0;
}

int Device::devoptab_mkdir(const char *path, int mode) {
    const auto ret = webdav_mkdir(path);
    if (ret < 0) {
        log_write("[WEBDAV] webdav_mkdir() failed: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    return 0;
}

int Device::devoptab_rmdir(const char *path) {
    const auto ret = webdav_rmdir(path);
    if (ret < 0) {
        log_write("[WEBDAV] webdav_rmdir() failed: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    return 0;
}

int Device::devoptab_diropen(void* fd, const char *path) {
    auto dir = static_cast<Dir*>(fd);

    auto entries = new DirEntries();
    const auto ret = webdav_dirlist(path, *entries);
    if (ret < 0) {
        log_write("[WEBDAV] webdav_dirlist() failed: %s errno: %s\n", path, std::strerror(-ret));
        delete entries;
        return ret;
    }

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

    filestat->st_nlink = 1;
    std::strcpy(filename, entry.name.c_str());

    dir->index++;
    return 0;
}

int Device::devoptab_dirclose(void* fd) {
    auto dir = static_cast<Dir*>(fd);

    delete dir->entries;
    return 0;
}

int Device::devoptab_lstat(const char *path, struct stat *st) {
    auto ret = webdav_stat(path, st, false);
    if (ret == -ENOENT) {
        ret = webdav_stat(path, st, true);
    }

    if (ret < 0) {
        log_write("[WEBDAV] webdav_stat() failed: %s errno: %s\n", path, std::strerror(-ret));
        return ret;
    }

    return 0;
}

int Device::devoptab_ftruncate(void *fd, off_t len) {
    auto file = static_cast<File*>(fd);

    if (!file->write_mode) {
        log_write("[WEBDAV] Attempt to truncate a read-only file\n");
        return -EBADF;
    }

    file->entry->st.st_size = len;
    return 0;
}

int Device::devoptab_fsync(void *fd) {
    auto file = static_cast<File*>(fd);

    if (!file->write_mode) {
        log_write("[WEBDAV] Attempt to fsync a read-only file\n");
        return -EBADF;
    }

    return 0;
}

} // namespace

Result MountWebdavAll() {
    return common::MountNetworkDevice([](const common::MountConfig& config) {
            return std::make_unique<Device>(config);
        },
        sizeof(File), sizeof(Dir),
        "WEBDAV"
    );
}

} // namespace sphaira::devoptab
