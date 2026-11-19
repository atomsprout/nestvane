#include "DynamicProbe.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--art") {
        const auto result = nestvane::probeArt(argv[2]);
        std::cout << result.report;
        return result.passed ? 0 : 1;
    }
    if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--init") {
        auto result = nestvane::probeInit(argv[2], argc == 4 ? argv[3] : "");
        std::cout << result.report;
        return result.passed ? 0 : 1;
    }
    auto result = nestvane::probeDynamic(argc > 1 ? argv[1] : "generated/guests");
    std::cout << result.report;
    return result.passed ? 0 : 1;
}
