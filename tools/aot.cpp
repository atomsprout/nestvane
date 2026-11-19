#include "Runtime.hpp"

#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace nestvane;

namespace {

std::string reg(unsigned index) {
    return "x[" + std::to_string(index) + "]";
}

std::string imm(uint64_t value) {
    return std::to_string(value) + "ULL";
}

void emitInstruction(const Instruction& op, size_t index) {
    auto rd = reg(op.rd);
    auto rn = reg(op.rn);
    auto offset = imm(op.immediate);
    switch (op.op) {
        case Op::addImm:
            std::cout << "        " << rd << " = " << rn << " + " << offset << ";\n";
            break;
        case Op::subImm:
            std::cout << "        " << rd << " = " << rn << " - " << offset << ";\n";
            break;
        case Op::addReg:
            std::cout << "        " << rd << " = " << rn << " + " << reg(op.rm) << ";\n";
            break;
        case Op::load:
        case Op::store: {
            auto address = rn + " + " + offset;
            auto method = op.op == Op::load ? "load64" : "store64";
            std::cout << "        if (!m." << method << '(' << address << ", " << rd << ")) {\n"
                      << "            cpu.retired += " << index << ";\n"
                      << "            m.fault(pc + " << index * 4 << ", " << address << ");\n"
                      << "            return;\n"
                      << "        }\n";
            break;
        }
        default:
            throw std::runtime_error("AOT only accepts the supported loop-body instructions");
    }
}

uint64_t emitBlock(const std::string& name, const Image& image, uint64_t start, uint64_t end) {
    if (start < image.entry || start % 4 || end % 4 || end <= start || end - start > 64 ||
        name.empty() || !std::isalpha(static_cast<unsigned char>(name.front()))) {
        throw std::runtime_error("invalid AOT region");
    }
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '_') throw std::runtime_error("invalid AOT name");
    }

    Machine machine(image);
    auto code = machine.read(start, end - start, 1);
    if (!code) throw std::runtime_error("AOT range is not executable");
    std::vector<uint32_t> words((end - start) / 4);
    std::memcpy(words.data(), code, end - start);
    auto branch = decode(words.back());
    if (branch.op != Op::cbnz || end - 4 + branch.immediate != start) {
        throw std::runtime_error("AOT only accepts a loop ending in CBNZ back to its entry");
    }

    std::cout << "constexpr uint32_t " << name << "_words[] = {";
    for (auto word : words) std::cout << imm(word) << ", ";
    std::cout << "};\n"
              << "void " << name << "_entry(Machine& m, uint64_t pc) {\n"
              << "    auto& cpu = m.cpu;\n"
              << "    auto& x = cpu.x;\n"
              << "    do {\n";
    for (size_t i = 0; i + 1 < words.size(); ++i) emitInstruction(decode(words[i]), i);
    std::cout << "        cpu.retired += " << words.size() << ";\n"
              << "        if (!" << reg(branch.rd) << ") { cpu.pc = pc + "
              << end - start << "; return; }\n"
              << "    } while (fuel - cpu.retired >= " << words.size() << ");\n"
              << "}\n";
    return start - image.entry;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 5 || (argc - 1) % 4) throw std::runtime_error("usage: aot name elf start end [...]");
    std::cout << "// Generated from the linked guest ELF files.\n"
              << "#include \"Runtime.hpp\"\n\nnamespace nestvane {\nnamespace {\n";
    std::vector<std::pair<std::string, uint64_t>> entries;
    for (int i = 1; i < argc; i += 4) {
        auto image = loadELF(readFile(argv[i + 1]));
        auto offset = emitBlock(argv[i], image, std::stoull(argv[i + 2], nullptr, 16),
                                std::stoull(argv[i + 3], nullptr, 16));
        entries.emplace_back(argv[i], offset);
    }
    std::cout << "constexpr AOTBlock table[] = {\n";
    for (const auto& [name, offset] : entries) {
        std::cout << "    {\"" << name << "\", " << imm(offset) << ", " << name << "_words, "
                  << name << "_entry},\n";
    }
    std::cout << "};\n} // namespace\n"
              << "std::span<const AOTBlock> compiledBlocks() { return table; }\n"
              << "} // namespace nestvane\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
