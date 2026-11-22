// The name server behind the guest's 10.0.2.3: queries are parsed, answered from the system's
// resolver and written back, over the loopback, and the guest's addresses map to it.
#include "HostSocket.hpp"

#include <cstdio>
#include <cstdlib>

using namespace nestvane;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s: %s\n", what, condition ? "ok" : "FAILED");
    failures += !condition;
}

static std::vector<uint8_t> query(uint16_t id, std::initializer_list<const char*> labels, uint16_t type, bool extended = false) {
    std::vector<uint8_t> out{uint8_t(id >> 8), uint8_t(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, uint8_t(extended)};
    for (const char* label : labels) {
        out.push_back(uint8_t(std::strlen(label)));
        out.insert(out.end(), label, label + std::strlen(label));
    }
    out.push_back(0);
    dnsproxy::append16(out, type);
    dnsproxy::append16(out, 1);
    if (extended) out.insert(out.end(), {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0});
    return out;
}

int main() {
    const auto asked = dnsproxy::parse(query(0x1234, {"www", "example", "com"}, 28, true));
    check(asked && asked->name == "www.example.com." && asked->type == 28 && asked->id == 0x1234 && asked->extended &&
          asked->answerLimit == 4096, "a query is read");
    check(!dnsproxy::parse(std::vector<uint8_t>{1, 2, 3}), "a short packet is no query");
    auto response = query(1, {"a"}, 1);
    response[2] |= 0x80;
    check(!dnsproxy::parse(response), "a response is no query");

    // What the answer is built from comes back in the wire's form, and what does not fit is left out and marked.
    const dnsproxy::Record alias{"www.example.com.", 5, 1, 300, {3, 'c', 'd', 'n', 0}}, address{"cdn.", 1, 1, 60, {192, 0, 2, 7}};
    const std::vector<dnsproxy::Record> both{alias, address};
    const auto plain = dnsproxy::parse(query(7, {"www", "example", "com"}, 1));
    const auto built = dnsproxy::respond(*plain, both, 0);
    check(built.size() == 12 + plain->wire.size() + (17 + 10 + 5) + (5 + 10 + 4) && built[0] == 0 && built[1] == 7 &&
          (built[2] & 0x80) && dnsproxy::read16(built, 6) == 2 && !std::memcmp(built.data() + built.size() - 4, "\xc0\x00\x02\x07", 4),
          "an answer carries the records");
    std::vector<dnsproxy::Record> many(40, dnsproxy::Record{"www.example.com.", 16, 1, 1, std::vector<uint8_t>(40, 'x')});
    const auto cut = dnsproxy::respond(*plain, many, 0);
    check(cut.size() <= 512 && (cut[2] & 0x02) && dnsproxy::read16(cut, 6) < 40, "an answer too long is cut and says so");

    // Names of the local network are not looked up at all.
    const auto local = dnsproxy::answer(query(9, {"printer", "local"}, 1));
    check(local && (local->at(3) & 0x0f) == 3 && dnsproxy::read16(*local, 6) == 0, "a .local name is refused without asking");

    // Through the loopback, as the guest's packets go: 10.0.2.3:53 is the server, and it answers as 10.0.2.3:53.
    hostnet::Address server{};
    server[0] = 2;
    server[3] = 53;
    std::memcpy(server.data() + 4, hostnet::nameServer.data(), 4);
    sockaddr_storage host{};
    hostnet::toHost(server, host);
    const auto& mapped = reinterpret_cast<const sockaddr_in&>(host);
    check(mapped.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(mapped.sin_port) == dnsproxy::port() && dnsproxy::port(),
          "the guest's name server is the one on the loopback");
    server[3] = 99;
    hostnet::toHost(server, host);
    check(ntohs(reinterpret_cast<const sockaddr_in&>(host).sin_port) == 1, "other ports of it lead nowhere");
    server[3] = 53;
    const int fd = hostnet::openDatagram(server);
    const auto question = query(0x4242, {"localhost"}, 1);
    check(hostnet::sendTo(fd, question, server) == int64_t(question.size()), "a query is sent");
    pollfd entry{fd, POLLIN, 0};
    ::poll(&entry, 1, 10000);
    std::array<uint8_t, 4096> buffer{};
    hostnet::Address source{};
    const auto received = hostnet::receiveFrom(fd, buffer, 2, source);
    check(received > 12 && dnsproxy::read16(buffer, 0) == 0x4242 && (buffer[2] & 0x80), "an answer comes back");
    check(source[2] == 0 && source[3] == 53 && !std::memcmp(source.data() + 4, hostnet::nameServer.data(), 4), "it comes from 10.0.2.3:53");
    check(received > 16 && dnsproxy::read16(buffer, 6) >= 1 && !std::memcmp(buffer.data() + received - 4, "\x7f\x00\x00\x01", 4),
          "localhost is 127.0.0.1");
    ::close(fd);

    // What is not carried out at all.
    const auto destination = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        hostnet::Address address{};
        address[0] = 2;
        address[4] = a, address[5] = b, address[6] = c, address[7] = d;
        return address;
    };
    check(hostnet::unreachable(destination(224, 0, 0, 251)) && hostnet::unreachable(destination(239, 255, 255, 250)) &&
          hostnet::unreachable(destination(255, 255, 255, 255)), "multicast and broadcast stay in");
    check(hostnet::unreachable(destination(10, 0, 2, 2)) && !hostnet::unreachable(destination(10, 0, 2, 3)), "so does the guest's own subnet, but for the name server");
    check(!hostnet::unreachable(destination(192, 168, 1, 10)) && !hostnet::unreachable(destination(1, 1, 1, 1)), "other addresses go out");
    std::printf("failures=%d\n", failures);
    return failures != 0;
}
