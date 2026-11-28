#include <rea_jackcess/pgeo/layer_reader.hpp>
#include <rea_jackcess/version.hpp>

#include <iostream>
#include <string_view>

int main() {
    const std::string_view version = rea_jackcess::version();
    if (version.empty()) {
        std::cerr << "installed library reported an empty version\n";
        return 1;
    }
    std::cout << "rea-jackcess " << version << "\n";
    return 0;
}
