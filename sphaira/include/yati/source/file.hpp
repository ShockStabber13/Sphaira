#pragma once

#include "base.hpp"
#include "fs.hpp"
#include <switch.h>
#include <memory>
#include <stop_token>

namespace sphaira::yati::source {

struct File final : Base {
    File(fs::Fs* fs, const fs::FsPath& path, std::stop_token token = {});
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;
    Result GetSize(s64* out);

private:
    fs::Fs* m_fs{};
    fs::File m_file{};
};

} // namespace sphaira::yati::source
