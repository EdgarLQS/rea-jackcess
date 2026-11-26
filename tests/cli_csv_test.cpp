// The CLI export contract: `--header` adds the column-name row, the flag is optional, and an
// unknown option is rejected instead of being ignored. The expected text is the fixed Jackcess
// 5.2.0 ExportUtil output for the same table.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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
                ("rea-jackcess-cli-csv-" + std::to_string(stamp));
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string quote(const std::filesystem::path& path) { return "'" + path.string() + "'"; }

// std::system reports the wait status, so the exit code has to be extracted on POSIX.
int run(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    return (status >> 8) & 0xFF;
}

const std::string expected_body =
    "row1,1,,,1,1\nrow2,2,row2-memo,row2-memo,2,2\n"
    "row3,3,row3-memo,row3-memo-again,3,3\nrow4,4,row4-memo,row4-memo,4,4\n";

const std::string expected_header =
    "id,VersionHistory_F5F8918F-0A3F-4DA9-AE71-184EE5012880,memo-data,append-memo-data,"
    "multi-value-data,attach-data\n";

void test_export(const std::filesystem::path& cli, const std::filesystem::path& fixture) {
    const TemporaryDirectory directory;

    const auto without_header_path = directory.file("no-header.csv");
    check(run(quote(cli) + " export " + quote(fixture) + " Table1 " +
              quote(without_header_path)) == 0,
          "export without --header failed");
    check(read_file(without_header_path) == expected_body,
          "export without --header changed the CSV body:\n" + read_file(without_header_path));

    // The optional flag is accepted after the file argument, which is what the usage line says.
    const auto with_header_path = directory.file("header.csv");
    check(run(quote(cli) + " export " + quote(fixture) + " Table1 " + quote(with_header_path) +
              " --header") == 0,
          "export with --header failed");
    const auto with_header = read_file(with_header_path);
    check(with_header == expected_header + expected_body,
          "export with --header did not write the column names:\n" + with_header);

    // An unknown option is an argument error, not a silently ignored word.
    const auto rejected_path = directory.file("rejected.csv");
    check(run(quote(cli) + " export " + quote(fixture) + " Table1 " + quote(rejected_path) +
              " --bogus 2>/dev/null") == 2,
          "an unknown export option should exit with code 2");
    check(!std::filesystem::exists(rejected_path),
          "a rejected export still created the CSV file");
}

}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: cli_csv <rea-jackcess> <complex-data-v2007.accdb>\n";
        return 2;
    }
    const std::filesystem::path cli(argv[1]);
    const std::filesystem::path fixture(argv[2]);
    if (!std::filesystem::exists(cli) || !std::filesystem::exists(fixture)) {
        std::cout << "CLI or fixture is missing\n";
        return 77;
    }
    try {
        test_export(cli, fixture);
        std::cout << "CLI CSV export contract holds\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
