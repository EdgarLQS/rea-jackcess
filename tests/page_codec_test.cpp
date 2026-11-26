// A read-only handle can only use an encoded database through a caller-supplied page codec. The
// test scrambles a database it just created, so no committed sample is needed: page zero stays
// plain and declares the encoding, pages 1..n are XOR-scrambled, and a codec reverses that.

#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/page_codec.hpp"

#include "format/jet_format.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFile;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::DatabaseOptions;
using rea_jackcess::DataType;
using rea_jackcess::PageCodec;
using rea_jackcess::PageCodecContext;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

constexpr std::uint8_t scramble_key = 0x5A;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-codec-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "could not write " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "could not write " + path.string());
}

Table find_table(const Database& database, const std::string& name) {
    for (auto table : database.tables()) {
        if (table.is_user_table() && table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("missing table " + name);
}

// Builds a plain V2000 database with three rows, opens it read-write for the inserts, and leaves
// the file closed with nothing else holding it.
void create_plain_database(const std::filesystem::path& path) {
    Database::create(path, DatabaseFormat::v2000,
                     {TableBuilder("Items")
                          .add_column("Key", DataType::int32)
                          .add_column("Label", DataType::text)});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Items");
    table.add_rows({{std::int32_t{1}, std::string("alpha")},
                    {std::int32_t{2}, std::string("beta")},
                    {std::int32_t{3}, std::string("gamma")}});
}

// Returns the stored file with pages 1..n XOR-scrambled and the header declaring an encoding key.
// Page zero otherwise stays byte-identical, mirroring Access, which never encodes the header.
std::vector<std::uint8_t> scramble(const std::vector<std::uint8_t>& plain, std::uint32_t page_size) {
    std::vector<std::uint8_t> scrambled = plain;
    check(scrambled.size() >= page_size * 2, "the fixture should have at least one data page");
    for (std::size_t offset = page_size; offset < scrambled.size(); ++offset) {
        scrambled[offset] ^= scramble_key;
    }

    const std::size_t key_offset = rea_jackcess::detail::encoding_key_offset;
    check(scrambled.size() >= key_offset + 4, "the header should hold the encoding key");
    // Any non-zero key marks the pages as encoded; the value itself is opaque to the reader.
    const std::uint8_t key_bytes[4] = {0x11, 0x22, 0x33, 0x44};
    for (std::size_t index = 0; index < 4; ++index) {
        scrambled[key_offset + index] =
            key_bytes[index] ^ rea_jackcess::detail::header_mask_byte(key_offset + index);
    }
    return scrambled;
}

// Reverses scramble() and records which pages it was asked about.
class XorCodec final : public PageCodec {
public:
    void decode_page(std::uint8_t* page, std::size_t size, std::uint64_t page_number) override {
        pages.push_back(page_number);
        sizes.push_back(size);
        for (std::size_t index = 0; index < size; ++index) {
            page[index] ^= scramble_key;
        }
    }

    std::vector<std::uint64_t> pages;
    std::vector<std::size_t> sizes;
};

class ThrowingCodec final : public PageCodec {
public:
    void decode_page(std::uint8_t*, std::size_t, std::uint64_t) override {
        throw std::runtime_error("boom");
    }
};

class ContextErrorCodec final : public PageCodec {
public:
    void decode_page(std::uint8_t*, std::size_t, std::uint64_t) override {
        throw DatabaseError(DatabaseErrorCode::corrupted_file, "codec rejected the page", {}, 7);
    }
};

template <typename Callable>
DatabaseError expect_database_error(Callable&& callable, const std::string& what) {
    try {
        callable();
    } catch (const DatabaseError& error) {
        return error;
    }
    throw std::runtime_error(what + ": no error was reported");
}

void verify_rows(const Database& database) {
    auto table = find_table(database, "Items");
    auto cursor = table.rows();
    const std::vector<std::string> expected{"alpha", "beta", "gamma"};
    std::size_t index = 0;
    while (auto row = cursor.next()) {
        check(index < expected.size(), "decoded database has more rows than expected");
        check(std::get<std::int32_t>(row->at(0)) == static_cast<std::int32_t>(index + 1),
              "decoded key changed");
        check(std::get<std::string>(row->at(1)) == expected[index], "decoded label changed");
        ++index;
    }
    check(index == expected.size(), "decoded database is missing rows");
}

void test_plain_and_scrambled_detection(const TemporaryDirectory& directory) {
    const auto plain_path = directory.file("plain.mdb");
    create_plain_database(plain_path);

    const auto plain_bytes = read_file(plain_path);
    const DatabaseFile plain = DatabaseFile::open(plain_path);
    check(!plain.is_encoded(), "a freshly created database should not report encoding");
    check(plain.page_count() >= 2, "the fixture should hold more than the header page");

    const auto scrambled_bytes = scramble(plain_bytes, plain.format().page_size);
    const auto scrambled_path = directory.file("scrambled.mdb");
    write_file(scrambled_path, scrambled_bytes);
    check(read_file(scrambled_path) == scrambled_bytes, "the scrambled copy did not round-trip");

    check(DatabaseFile::open(scrambled_path).is_encoded(),
          "a non-zero encoding key should be detected");
}

void test_missing_codec_is_rejected(const TemporaryDirectory& directory) {
    const auto scrambled_path = directory.file("scrambled.mdb");
    const auto scrambled_bytes = read_file(scrambled_path);

    const auto path_error = expect_database_error(
        [&] { (void)Database::open(scrambled_path); }, "an encoded file opened without a codec");
    check(path_error.code() == DatabaseErrorCode::unsupported_operation,
          "an encoded file without a codec should be unsupported");
    check(std::string(path_error.what()).find("encoded") != std::string::npos,
          "the error should say the pages are encoded");
    check(path_error.path() == scrambled_path, "the error should carry the database path");

    const auto mode_error = expect_database_error(
        [&] { (void)Database::open(scrambled_path, DatabaseOpenMode::read_only); },
        "an encoded file opened through the access-mode overload");
    check(mode_error.code() == DatabaseErrorCode::unsupported_operation,
          "the access-mode overload should reject encoded pages too");

    const auto memory_error = expect_database_error(
        [&] { (void)Database::open(scrambled_bytes); }, "an encoded buffer opened without a codec");
    check(memory_error.code() == DatabaseErrorCode::unsupported_operation,
          "an encoded buffer without a codec should be unsupported");
    check(memory_error.path() == "<memory>", "the memory open should report <memory>");

    // A factory that declines the file must not be treated as a successful decode.
    DatabaseOptions declined;
    declined.page_codec = [](const PageCodecContext&) -> std::shared_ptr<PageCodec> { return {}; };
    const auto declined_error = expect_database_error(
        [&] { (void)Database::open(scrambled_path, declined); }, "a declined encoded file");
    check(declined_error.code() == DatabaseErrorCode::unsupported_operation,
          "a declined encoded file should be unsupported");
    check(std::string(declined_error.what()).find("no codec") != std::string::npos,
          "the error should report that the factory returned no codec");
}

void test_configured_codec_decodes(const TemporaryDirectory& directory) {
    const auto plain_path = directory.file("plain.mdb");
    const auto scrambled_path = directory.file("scrambled.mdb");
    const auto plain_bytes = read_file(plain_path);
    const auto scrambled_bytes = read_file(scrambled_path);

    auto codec = std::make_shared<XorCodec>();
    int factory_calls = 0;
    bool saw_encoded_flag = false;

    DatabaseOptions options;
    options.page_codec = [&](const PageCodecContext& context) -> std::shared_ptr<PageCodec> {
        ++factory_calls;
        saw_encoded_flag = context.encoded;
        check(context.format.version == rea_jackcess::EngineVersion::jet4,
              "the codec context should describe the identified format");
        check(context.path == scrambled_path, "the codec context should report the database path");
        // The header stays readable before the codec exists.
        const auto root = context.read_raw_page(0);
        check(root.size() == context.format.page_size, "the raw reader should return one whole page");
        // The stored header is readable before any page is decoded; only the encoding key differs
        // from the plain file it was scrambled from.
        const std::size_t key_offset = rea_jackcess::detail::encoding_key_offset;
        bool header_matches = root[20] == plain_bytes[20];
        for (std::size_t offset = 0; offset < root.size() && header_matches; ++offset) {
            if (offset >= key_offset && offset < key_offset + 4) {
                continue;
            }
            header_matches = root[offset] == plain_bytes[offset];
        }
        check(header_matches, "the raw reader should expose the stored header");
        return codec;
    };

    auto database = Database::open(scrambled_path, options);
    check(factory_calls == 1, "the codec factory should run exactly once per open");
    check(saw_encoded_flag, "the codec context should report the declared encoding");
    verify_rows(database);

    check(!codec->pages.empty(), "the codec should have been asked to decode pages");
    const std::uint64_t page_count = DatabaseFile::open(scrambled_path).page_count();
    for (const auto page_number : codec->pages) {
        check(page_number != 0, "page zero must never be passed to the codec");
        check(page_number < page_count, "the codec should only receive existing pages");
    }
    for (const auto size : codec->sizes) {
        check(size == database.format().page_size, "the codec should receive whole pages");
    }

    // The same bytes decode identically from memory.
    auto memory_codec = std::make_shared<XorCodec>();
    DatabaseOptions memory_options;
    memory_options.page_codec = [&](const PageCodecContext&) -> std::shared_ptr<PageCodec> {
        return memory_codec;
    };
    auto memory_database = Database::open(scrambled_bytes, memory_options);
    check(memory_database.path() == "<memory>", "the memory handle should report <memory>");
    verify_rows(memory_database);

    // A plain database opens with or without a codec; declining it keeps the stored bytes.
    DatabaseOptions plain_options;
    plain_options.page_codec = [](const PageCodecContext& context) -> std::shared_ptr<PageCodec> {
        check(!context.encoded, "a plain database should not report encoding");
        return {};
    };
    auto plain_database = Database::open(plain_path, plain_options);
    verify_rows(plain_database);
}

void test_always_encoded_container_is_rejected(const TemporaryDirectory& directory) {
    // An MSISAM container is encoded even when its key field is zero, so a bare open has to be
    // refused instead of parsed as Jet4 pages.
    const auto msisam_path = directory.file("msisam.mdb");
    std::vector<std::uint8_t> bytes(4096 * 2, 0);
    const std::string engine_name = "MSISAM Database";
    std::copy(engine_name.begin(), engine_name.end(), bytes.begin() + 4);
    bytes[20] = 1;
    write_file(msisam_path, bytes);

    const DatabaseFile msisam = DatabaseFile::open(msisam_path);
    check(msisam.format().version == rea_jackcess::EngineVersion::msisam,
          "the container should be identified as MSISAM");
    check(msisam.is_encoded(), "an MSISAM container should always report encoding");

    const auto error = expect_database_error(
        [&] { (void)Database::open(msisam_path); }, "an MSISAM container opened without a codec");
    check(error.code() == DatabaseErrorCode::unsupported_operation,
          "an MSISAM container without a codec should be unsupported");
    check(error.path() == msisam_path, "the refusal should name the container");
}

void test_codec_failures_propagate(const TemporaryDirectory& directory) {
    const auto scrambled_path = directory.file("scrambled.mdb");

    DatabaseOptions throwing;
    throwing.page_codec = [](const PageCodecContext&) -> std::shared_ptr<PageCodec> {
        return std::make_shared<ThrowingCodec>();
    };
    const auto thrown = expect_database_error(
        [&] { (void)Database::open(scrambled_path, throwing); }, "a codec that throws");
    check(thrown.code() == DatabaseErrorCode::corrupted_file,
          "an unexpected codec exception should be reported as a corrupt decode");
    check(thrown.page_number().has_value() && *thrown.page_number() >= 1,
          "the decode failure should name the page that failed");
    check(std::string(thrown.what()).find("boom") != std::string::npos,
          "the decoder's message should be preserved");
    check(thrown.path() == scrambled_path, "the decode failure should name the file");

    DatabaseOptions context_error;
    context_error.page_codec = [](const PageCodecContext&) -> std::shared_ptr<PageCodec> {
        return std::make_shared<ContextErrorCodec>();
    };
    const auto reported = expect_database_error(
        [&] { (void)Database::open(scrambled_path, context_error); },
        "a codec that reports a DatabaseError");
    check(reported.code() == DatabaseErrorCode::corrupted_file, "the codec's code should be kept");
    check(reported.page_number() == std::uint64_t{7},
          "a codec's own page number should not be overwritten");
    check(std::string(reported.what()) == "codec rejected the page",
          "the codec's own message should be kept");
}

}

int main() {
    try {
        const TemporaryDirectory directory;
        test_plain_and_scrambled_detection(directory);
        test_missing_codec_is_rejected(directory);
        test_configured_codec_decodes(directory);
        test_always_encoded_container_is_rejected(directory);
        test_codec_failures_propagate(directory);
        std::cout << "page codec contract holds\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
