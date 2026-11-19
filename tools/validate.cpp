#include "Validation.hpp"

#include <iostream>

int main(int argc, char** argv) {
    auto result = nestvane::validate(argc > 1 ? argv[1] : "generated/guests");
    std::cout << result.report;
    return result.passed ? 0 : 1;
}
