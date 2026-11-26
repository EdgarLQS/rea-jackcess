// The Jet3 code-page tables replay Java NIO decoding, which is what the fixed Jackcess 5.2.0
// uses for legacy text. The expected strings below come from authoritative sources (Unicode code
// charts and Python's codecs, cross-checked at generation time), not from the generator.

#include "rea_jackcess/database.hpp"
#include "util/code_page.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::detail::decode_code_page_text;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string decode(std::initializer_list<std::uint8_t> bytes, std::uint16_t code_page = 1252) {
    return decode_code_page_text({bytes.begin(), bytes.end()}, code_page);
}

void test_single_byte_pages() {
    // Windows-1252 keeps its historical behaviour.
    check(decode({0x80}) == "\xe2\x82\xac", "1252 did not map 0x80 to the euro sign");
    check(decode({0x41, 0xC3}) == "A\xc3\x83", "1252 high bytes changed");
    // Latin-1 is the identity map; 819 and 28591 are the same page.
    check(decode({0x80}, 819) == "\xc2\x80", "819 did not keep 0x80");
    check(decode({0xE9}, 28591) == "\xc3\xa9", "28591 did not keep 0xE9");

    // Windows-1251: Cyrillic. П=CF р=F0 и=E8 в=E2 е=E5 т=F2; 0x98 is unmappable.
    check(decode({0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2}, 1251) == "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82",
          "1251 did not decode Cyrillic text");
    check(decode({0xDF}, 1251) == "\xd0\xaf", "1251 did not map 0xDF to Я");
    check(decode({0xFF}, 1251) == "\xd1\x8f", "1251 did not map 0xFF to я");
    check(decode({0x98}, 1251) == "\xef\xbf\xbd", "1251 did not replace the unmappable 0x98");
    // Russian word "Данные" (data): Д=C4 а=E0 н=ED н=ED ы=FB е=E5.
    check(decode({0xC4, 0xE0, 0xED, 0xED, 0xFB, 0xE5}, 1251) ==
              "\xd0\x94\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5",
          "1251 did not decode Данные");
}

void test_gbk() {
    // GBK double bytes: 中=D6D0 文=CEC4 数=CAFD 据=BEDD 库=BFE2 日=C8D5 本=B1BE.
    check(decode({0xD6, 0xD0, 0xCE, 0xC4}, 936) == "\xe4\xb8\xad\xe6\x96\x87",
          "936 did not decode 中文");
    check(decode({0xCA, 0xFD, 0xBE, 0xDD, 0xBF, 0xE2}, 936) == "\xe6\x95\xb0\xe6\x8d\xae\xe5\xba\x93",
          "936 did not decode 数据库");
    check(decode({0xC8, 0xD5, 0xB1, 0xBE}, 936) == "\xe6\x97\xa5\xe6\x9c\xac",
          "936 did not decode 日本");
    // ASCII passes through, including inside mixed text.
    check(decode({0x44, 0x42, 0x2D, 0xCA, 0xFD, 0xBE, 0xDD, 0xBF, 0xE2}, 936) ==
              "DB-\xe6\x95\xb0\xe6\x8d\xae\xe5\xba\x93",
          "936 did not keep ASCII next to double bytes");
    // 0x80 is a lead byte in Java's GBK charset (the euro sign lives in GB18030, not GBK), so a
    // lone 0x80 is malformed.
    check(decode({0x80}, 936) == "\xef\xbf\xbd", "936 did not replace a lone 0x80");
    // GB2312 punctuation: ，=A3AC (U+FF0C) and 。=A1A3 (U+3002).
    check(decode({0xA3, 0xAC}, 936) == "\xef\xbc\x8c", "936 did not decode full-width comma");
    check(decode({0xA1, 0xA3}, 936) == "\xe3\x80\x82", "936 did not decode the ideographic stop");
}

void test_gbk_malformed() {
    const std::string replacement = "\xef\xbf\xbd";
    // A lead at the end of the buffer is one malformed run.
    check(decode({0xD6}, 936) == replacement, "936 did not replace a trailing lead");
    // An invalid trail replaces the lead and then re-decodes the trail (Java REPLACE semantics).
    check(decode({0xD6, 0x20}, 936) == replacement + " ",
          "936 did not replace an invalid trail and re-decode it");
    check(decode({0xD6, 0x7F}, 936) == replacement + "\x7f",
          "936 did not re-decode the 0x7F trail");
    // 0xFF swallows the whole pair, verified against Java's GBK decoder.
    check(decode({0x81, 0xFF}, 936) == replacement, "936 did not replace the 0xFF pair");
    // Every high byte can lead a pair: (0xA0, 0x41) decodes to U+71D7 per Java's GBK charset.
    check(decode({0xA0, 0x41}, 936) == "\xe7\x87\x97", "936 did not decode the 0xA0 lead");
    // The same byte alone is malformed.
    check(decode({0xA0}, 936) == replacement, "936 did not replace a lone 0xA0");
    // Concatenation: decoding a stream equals decoding its pieces, because every run is replaced
    // independently.
    const std::vector<std::uint8_t> stream{0xD6, 0xD0, 0x20, 0xD6, 0x7F, 0xCE, 0xC4, 0xFF};
    const std::string whole = decode_code_page_text(stream, 936);
    std::string piecewise;
    std::size_t offset = 0;
    for (const std::size_t length : {std::size_t{3}, std::size_t{2}, std::size_t{2}, std::size_t{1}}) {
        piecewise += decode_code_page_text({stream.begin() + offset,
                                            stream.begin() + offset + length}, 936);
        offset += length;
    }
    check(whole == piecewise, "936 decoding is not position-independent");
    const std::string expected = std::string("\xe4\xb8\xad") + " " + replacement + "\x7f"
        + "\xe6\x96\x87" + replacement;
    check(whole == expected, "936 stream decoding produced unexpected text: " + whole);
}

void test_rejections_and_options(const std::filesystem::path& sample) {
    bool rejected = false;
    try {
        (void)decode({0x41}, 1254);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "an unmapped code page did not throw");

    if (sample.empty()) {
        return;
    }
    // The open path accepts the new overrides and still rejects unknown pages; names are ASCII so
    // a Jet3 file reads identically under any supported page.
    const std::filesystem::path& path = sample;
    for (const std::uint16_t page : {std::uint16_t{936}, std::uint16_t{1251}}) {
        rea_jackcess::DatabaseOptions options;
        options.code_page = page;
        auto database = rea_jackcess::Database::open(path, options);
        check(database.tables().size() == 10, "code page override changed the table list");
    }
    rea_jackcess::DatabaseOptions unsupported;
    unsupported.code_page = 1254;
    bool open_rejected = false;
    try {
        (void)rea_jackcess::Database::open(path, unsupported);
    } catch (const rea_jackcess::DatabaseError& error) {
        open_rejected = error.code() == rea_jackcess::DatabaseErrorCode::unsupported_operation;
    }
    check(open_rejected, "an unsupported code page override was accepted");
}

}

int main(int argc, char** argv) {
    try {
        test_single_byte_pages();
        test_gbk();
        test_gbk_malformed();
        test_rejections_and_options(argc > 1 ? std::filesystem::path(argv[1])
                                             : std::filesystem::path{});
        std::cout << "code page tables hold\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
