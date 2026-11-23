#include "value/attachment_value.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_rejected(const std::vector<std::uint8_t>& encoded, const char* message) {
    try {
        (void)rea_jackcess::detail::decode_attachment_file_data(encoded);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

}

int main() {
    try {
        std::vector<std::uint8_t> raw_wrapper;
        append_u32(raw_wrapper, 0);
        append_u32(raw_wrapper, 8);
        append_u32(raw_wrapper, 4);
        raw_wrapper.insert(raw_wrapper.end(), {'d', 'a', 't', 'a'});
        const auto raw_data = rea_jackcess::detail::decode_attachment_file_data(raw_wrapper);
        check(std::string(raw_data.begin(), raw_data.end()) == "data",
              "raw attachment wrapper did not yield its file payload");

        check_rejected({0, 0, 0, 0, 0}, "truncated attachment wrapper was accepted");
        auto unknown_type = raw_wrapper;
        unknown_type[0] = 2;
        check_rejected(unknown_type, "unknown attachment wrapper type was accepted");
        auto invalid_content_header = raw_wrapper;
        invalid_content_header[8] = 9;
        check_rejected(invalid_content_header, "oversized attachment content header was accepted");
        std::cout << "attachment wrapper tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "attachment wrapper tests failed: " << error.what() << '\n';
        return 1;
    }
}
