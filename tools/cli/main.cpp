#include "rea_jackcess/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    const std::string_view option = argc == 2 ? argv[1] : "";
    if (argc == 1 || option == "--help") {
        std::cout << "Usage: rea-jackcess [--help | --version]\n";
        return 0;
    }
    if (option == "--version") {
        std::cout << "rea-jackcess " << rea_jackcess::version() << '\n';
        return 0;
    }

    std::cerr << "Invalid arguments. Run rea-jackcess --help for usage.\n";
    return 2;
}
