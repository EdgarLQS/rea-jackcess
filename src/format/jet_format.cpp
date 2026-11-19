#include "jet_format.hpp"

#include <array>
#include <string>

namespace rea_jackcess {

const char* FormatInfo::name() const noexcept {
    switch (version) {
        case EngineVersion::jet3:
            return "Jet3";
        case EngineVersion::jet4:
            return "Jet4";
        case EngineVersion::ace2007:
            return "ACE2007";
        case EngineVersion::ace2010:
            return "ACE2010";
        case EngineVersion::ace2016:
            return "ACE2016";
        case EngineVersion::ace2019:
            return "ACE2019";
        case EngineVersion::msisam:
            return "MSISAM";
    }
    return "Unknown";
}

}

namespace rea_jackcess::detail {
namespace {

constexpr std::size_t engine_name_offset = 4;
constexpr std::uint64_t version_offset = 20;
constexpr std::array<char, 15> msisam_engine_name{
    'M', 'S', 'I', 'S', 'A', 'M', ' ', 'D', 'a', 't', 'a', 'b', 'a', 's', 'e'};

bool is_msisam(const std::array<std::uint8_t, database_header_size>& header) {
    for (std::size_t index = 0; index < msisam_engine_name.size(); ++index) {
        if (header[engine_name_offset + index] != static_cast<std::uint8_t>(msisam_engine_name[index])) {
            return false;
        }
    }
    return true;
}

}

FormatInfo detect_format(const std::array<std::uint8_t, database_header_size>& header,
                         const std::filesystem::path& path) {
    const std::uint8_t version = header[version_offset];
    switch (version) {
        case 0:
            return {EngineVersion::jet3, 2048, true};
        case 1:
            return {is_msisam(header) ? EngineVersion::msisam : EngineVersion::jet4, 4096, false};
        case 2:
            return {EngineVersion::ace2007, 4096, false};
        case 3:
            return {EngineVersion::ace2010, 4096, false};
        case 5:
            return {EngineVersion::ace2016, 4096, false};
        case 6:
            return {EngineVersion::ace2019, 4096, false};
        default:
            throw DatabaseError(DatabaseErrorCode::unsupported_version,
                                "Unsupported database engine version " + std::to_string(version), path);
    }
}

}
