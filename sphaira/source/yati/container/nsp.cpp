#include "yati/container/nsp.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "utils/open_diagnostics.hpp"
#include <limits>
#include <memory>
#include <cstring>

namespace sphaira::yati::container {
namespace {

#define PFS0_MAGIC 0x30534650

struct Pfs0Header {
    u32 magic;
    u32 total_files;
    u32 string_table_size;
    u32 padding;
};

struct Pfs0FileTableEntry {
    u64 data_offset;
    u64 data_size;
    u32 name_offset;
    u32 padding;
};

constexpr u32 MAX_PFS0_FILES = 0x10000;
constexpr u32 MAX_PFS0_STRING_TABLE_SIZE = 16 * 1024 * 1024;

bool CheckedAdd(u64 lhs, u64 rhs, u64& out) {
    if (rhs > std::numeric_limits<u64>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

Result ReadExact(source::Base* source, void* data, s64 offset, s64 size) {
    if (!size) {
        R_SUCCEED();
    }

    u64 bytes_read{};
    R_TRY(source->Read(data, offset, size, &bytes_read));
    if (bytes_read != static_cast<u64>(size)) {
        sphaira::open_diagnostics::nsp_read_short.store(true);
        sphaira::open_diagnostics::nsp_read_offset.store(static_cast<u64>(offset));
        sphaira::open_diagnostics::nsp_read_expected.store(static_cast<u64>(size));
        sphaira::open_diagnostics::nsp_read_received.store(bytes_read);
        R_THROW(Result_NspInvalidHeader);
    }
    R_SUCCEED();
}

// stdio-like wrapper for std::vector
struct BufHelper {
    BufHelper() = default;
    BufHelper(std::span<const u8> data) {
        write(data);
    }

    void write(const void* data, u64 size) {
        if (offset + size >= buf.size()) {
            buf.resize(offset + size);
        }
        std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }

    void write(std::span<const u8> data) {
        write(data.data(), data.size());
    }

    void seek(u64 where_to) {
        offset = where_to;
    }

    [[nodiscard]]
    auto tell() const {
        return offset;
    }

    std::vector<u8> buf;
    u64 offset{};
};

} // namespace

Result Nsp::GetCollections(Collections& out) {
    return GetCollections(out, 0, -1);
}

Result Nsp::GetCollections(Collections& out, s64 off) {
    return GetCollections(out, off, -1);
}

Result Nsp::GetCollections(Collections& out, s64 off, s64 container_size) {
    namespace diag = sphaira::open_diagnostics;
    diag::nsp_diag_valid.store(true);
    diag::nsp_files.store(0);
    diag::nsp_strings.store(0);
    diag::nsp_file_index.store(0);
    diag::nsp_read_short.store(false);
    diag::nsp_stage.store(1); // start offset
    R_UNLESS(off >= 0, Result_NspInvalidHeader);
    u64 container_end{};
    diag::nsp_stage.store(2); // container size and bounds
    if (container_size >= 0) {
        R_UNLESS(CheckedAdd(static_cast<u64>(off), static_cast<u64>(container_size), container_end), Result_NspInvalidHeader);
        R_UNLESS(container_end <= static_cast<u64>(std::numeric_limits<s64>::max()), Result_NspInvalidHeader);
    }

    Pfs0Header header{};
    diag::nsp_stage.store(3); // first 16-byte header
    R_TRY(ReadExact(m_source, std::addressof(header), off, sizeof(header)));
    diag::nsp_files.store(header.total_files);
    diag::nsp_strings.store(header.string_table_size);
    if (header.magic != PFS0_MAGIC) {
        // Determine whether a WebDAV/HTTP endpoint served real NSP bytes
        // or a response document, without logging any URL or credentials.
        sphaira::open_diagnostics::CaptureInvalidNspHeader(
            std::addressof(header), sizeof(header));
        R_THROW(Result_NspBadMagic);
    }
    diag::nsp_stage.store(4); // file count range
    R_UNLESS(header.total_files <= MAX_PFS0_FILES, Result_NspInvalidHeader);
    diag::nsp_stage.store(5); // string table length
    R_UNLESS(header.string_table_size <= MAX_PFS0_STRING_TABLE_SIZE, Result_NspInvalidHeader);

    const auto table_size = static_cast<u64>(header.total_files) * sizeof(Pfs0FileTableEntry);
    u64 table_offset{};
    u64 strings_offset{};
    u64 data_offset{};
    diag::nsp_stage.store(6); // partition offsets and bounds
    R_UNLESS(CheckedAdd(static_cast<u64>(off), sizeof(header), table_offset), Result_NspInvalidHeader);
    R_UNLESS(CheckedAdd(table_offset, table_size, strings_offset), Result_NspInvalidHeader);
    R_UNLESS(CheckedAdd(strings_offset, header.string_table_size, data_offset), Result_NspInvalidHeader);
    R_UNLESS(data_offset <= static_cast<u64>(std::numeric_limits<s64>::max()), Result_NspInvalidHeader);
    R_UNLESS(container_size < 0 || data_offset <= container_end, Result_NspInvalidHeader);
    m_data_offset = data_offset;

    std::vector<Pfs0FileTableEntry> file_table(header.total_files);
    diag::nsp_stage.store(7); // file table bytes
    R_TRY(ReadExact(m_source, file_table.data(), table_offset, table_size));

    std::vector<char> string_table(header.string_table_size);
    diag::nsp_stage.store(8); // string table bytes
    R_TRY(ReadExact(m_source, string_table.data(), strings_offset, string_table.size()));

    out.clear();
    out.reserve(header.total_files);
    size_t file_index = 0;
    for (const auto& file : file_table) {
        diag::nsp_file_index.store(file_index++);
        diag::nsp_stage.store(9); // file name offset
        R_UNLESS(file.name_offset < string_table.size(), Result_NspInvalidHeader);
        const auto name = string_table.data() + file.name_offset;
        const auto name_size = string_table.size() - file.name_offset;
        diag::nsp_stage.store(10); // null-terminated file name
        R_UNLESS(std::memchr(name, '\0', name_size), Result_NspInvalidHeader);

        u64 file_offset{};
        u64 file_end{};
        diag::nsp_stage.store(11); // file size/offset math
        R_UNLESS(CheckedAdd(data_offset, file.data_offset, file_offset), Result_NspInvalidHeader);
        R_UNLESS(CheckedAdd(file_offset, file.data_size, file_end), Result_NspInvalidHeader);
        R_UNLESS(file_end <= static_cast<u64>(std::numeric_limits<s64>::max()), Result_NspInvalidHeader);
        R_UNLESS(container_size < 0 || file_end <= container_end, Result_NspInvalidHeader);

        CollectionEntry entry;
        entry.name = name;
        entry.offset = file_offset;
        entry.size = file.data_size;
        out.emplace_back(entry);
    }

    diag::nsp_diag_valid.store(false);
    R_SUCCEED();
}

auto Nsp::Build(std::span<const CollectionEntry> entries, s64& size) -> std::vector<u8> {
    BufHelper buf;

    Pfs0Header header{};
    std::vector<Pfs0FileTableEntry> file_table(entries.size());
    std::vector<char> string_table;

    u64 string_offset{};
    u64 data_offset{};

    for (u32 i = 0; i < entries.size(); i++) {
        file_table[i].data_offset = data_offset;
        file_table[i].data_size = entries[i].size;
        file_table[i].name_offset = string_offset;
        file_table[i].padding = 0;

        string_table.resize(string_offset + entries[i].name.length() + 1);
        std::memcpy(string_table.data() + string_offset, entries[i].name.c_str(), entries[i].name.length() + 1);

        data_offset += file_table[i].data_size;
        string_offset += entries[i].name.length() + 1;
    }

    // Add padding to the string table so that the header as a whole is well-aligned
    const auto nameless_header_size = sizeof(Pfs0Header) + (file_table.size() * sizeof(Pfs0FileTableEntry));
    auto padded_string_table_size = ((nameless_header_size + string_table.size() + 0x1F) & ~0x1F) - nameless_header_size;

    // Add manual padding if the full Partition FS header would already be properly aligned.
    if (padded_string_table_size == string_table.size()) {
        padded_string_table_size += 0x20;
    }

    string_table.resize(padded_string_table_size);

    header.magic = PFS0_MAGIC;
    header.total_files = entries.size();
    header.string_table_size = string_table.size();
    header.padding = 0;

    buf.write(&header, sizeof(header));
    buf.write(file_table.data(), sizeof(Pfs0FileTableEntry) * file_table.size());
    buf.write(string_table.data(), string_table.size());

    // calculate nsp size.
    size = buf.tell();
    for (const auto& e : file_table) {
        size += e.data_size;
    }

    return buf.buf;
}

} // namespace sphaira::yati::container
