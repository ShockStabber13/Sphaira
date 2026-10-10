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
    bool write_mode;
};

struct Dir {
    DirEntries* entries;
    size_t index;
};

struct Device final : common::MountCurlDevice {
    using MountCurlDevice::MountCurlDevice;

private:
    // Only downloads use the stricter error handling; HEAD and PROPFIND keep
    // their existing WebDAV status-code handling.
    void curl_set_common_options(CURL* handle, const std::string& url) override;
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

// Never return HTTP error pages as file data. A stalled TorBox download
// must eventually fail so the read path can safely resume it.
void Device::curl_set_common_options(CURL* handle, const std::string& url) {
    common::MountCurlDevice::curl_set_common_options(handle, url);
    if (handle == transfer_curl) {
        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    }
}

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

    log_write("[WEBDAV] %s %s\n", cmd.c_str(), url.c_str());
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
        log_write("[WEBDAV] Attempt to read from a write-only file\n");
        return -EBADF;
    }

    // Avoid unsigned underflow when seeking to or past the end of the file.
    const auto total = file->entry->st.st_size;
    if (total <= 0 || file->off >= static_cast<size_t>(total)) {
        return 0;
    }
    len = std::min(len, static_cast<size_t>(total) - file->off);
    if (!len) {
        return 0;
    }

    if (file->off != file->last_off) {
        log_write("[WEBDAV] File offset changed from %zu to %zu, resetting download thread\n", file->last_off, file->off);
        file->last_off = file->off;
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;
    }

    constexpr int MAX_RETRIES = 3;
    constexpr u64 RETRY_WAIT_NS = 500'000'000ULL;
    size_t bytes_read = 0;
    int retries = 0;

    while (bytes_read < len) {
        if (!file->push_pull_thread_data) {
            file->push_pull_thread_data = CreatePushData(this->transfer_curl, build_url(file->entry->path, false), file->off);
            if (!file->push_pull_thread_data) {
                log_write("[WEBDAV] Failed to start download at offset %zu\n", file->off);
                return -EIO;
            }
        }

        const auto count = file->push_pull_thread_data->PullData(ptr + bytes_read, len - bytes_read);
        if (count) {
            bytes_read += count;
            file->off += count;
            file->last_off = file->off;
            retries = 0;
            continue;
        }

        // Incomplete transfer, including a clean-but-premature EOF. Never
        // report EOF while there are still bytes left in the known file.
        const auto http_code = file->push_pull_thread_data->code;
        const auto curl_failed = file->push_pull_thread_data->error;
        delete file->push_pull_thread_data;
        file->push_pull_thread_data = nullptr;

        if (http_code == 401 || http_code == 403) {
            log_write("[WEBDAV] Download unauthorized (HTTP %ld) at %zu\n", http_code, file->off);
            return -EACCES;
        }
        if (http_code == 404) {
            log_write("[WEBDAV] Download missing (HTTP 404) at %zu\n", file->off);
            return -ENOENT;
        }
        // Non-transient HTTP failures should be reported, not retried.
        if (http_code >= 400 && http_code != 429 && http_code != 500 &&
            http_code != 502 && http_code != 503 && http_code != 504) {
            log_write("[WEBDAV] Download rejected (HTTP %ld) at %zu\n", http_code, file->off);
            return -EIO;
        }

        if (++retries > MAX_RETRIES) {
            log_write("[WEBDAV] Exhausted retries at offset %zu (HTTP %ld, curl_error=%d)\n",
                file->off, http_code, curl_failed);
            return -EIO;
        }

        log_write("[WEBDAV] Short read at %zu; retry %d/%d (HTTP %ld, curl_error=%d)\n",
            file->off, retries, MAX_RETRIES, http_code, curl_failed);
        svcSleepThread(RETRY_WAIT_NS);
    }

    return bytes_read;
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
