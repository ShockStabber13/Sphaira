#include "yati/source/file.hpp"
#include "utils/devoptab_common.hpp"

namespace sphaira::yati::source {

File::File(fs::Fs* fs, const fs::FsPath& path, std::stop_token token) : m_fs{fs} {
    // This is scoped to fopen() on the current thread; the WebDAV file then
    // owns a copy of the token, including when its reads move to other threads.
    auto& current = devoptab::common::file_open_cancel_token;
    const auto previous = current;
    current = token;
    m_open_result = m_fs->OpenFile(path, FsOpenMode_Read, std::addressof(m_file));
    current = previous;
}

Result File::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    return m_file.Read(off, buf, size, 0, bytes_read);
}

Result File::GetSize(s64* out) {
    return m_file.GetSize(out);
}

} // namespace sphaira::yati::source
