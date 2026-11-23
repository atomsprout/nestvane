#pragma once

// The name server behind the guest's 10.0.2.3. It listens on the host's loopback and answers each
// query by asking the system's resolver (DNSServiceQueryRecord), so the names resolve the way they
// do for every other app on the phone: with the name servers of the network in use, VPN and private
// relay settings included. The app itself never sends to the network's name server, which is
// usually the router: iOS counts that as reaching the local network, and asks the user to allow it
// the moment Android looks up its first name.

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dns_sd.h>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace nestvane::dnsproxy {

struct Question {
    uint16_t id = 0;
    bool recursionDesired = false;
    std::string name;                 // escaped the way dns_sd takes it, with the final dot
    uint16_t type = 0, recordClass = 0;
    std::vector<uint8_t> wire;        // the question section as it came
    uint16_t answerLimit = 512;       // what the asker can take in one datagram
    bool extended = false;            // the query carried an OPT record
};

struct Record {
    std::string name;
    uint16_t type = 0, recordClass = 0;
    uint32_t timeToLive = 0;
    std::vector<uint8_t> data;
};

inline uint16_t read16(std::span<const uint8_t> bytes, size_t offset) {
    return uint16_t(bytes[offset] << 8 | bytes[offset + 1]);
}
inline void append16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

// One standard query with one question, or nothing.
inline std::optional<Question> parse(std::span<const uint8_t> packet) {
    if (packet.size() < 12) return std::nullopt;
    const uint16_t flags = read16(packet, 2);
    if ((flags & 0x8000) || (flags & 0x7800) || read16(packet, 4) != 1) return std::nullopt;
    Question question;
    question.id = read16(packet, 0);
    question.recursionDesired = flags & 0x0100;
    size_t offset = 12;
    for (;;) {
        if (offset >= packet.size()) return std::nullopt;
        const uint8_t length = packet[offset++];
        if (!length) break;
        if (length > 63 || offset + length > packet.size()) return std::nullopt;
        for (size_t index = 0; index < length; ++index) {
            const uint8_t byte = packet[offset + index];
            if (byte == '.' || byte == '\\') {
                question.name += '\\';
                question.name += char(byte);
            } else if (byte <= ' ' || byte >= 127) {
                char escaped[5];
                snprintf(escaped, sizeof(escaped), "\\%03u", byte);
                question.name += escaped;
            } else {
                question.name += char(byte);
            }
        }
        question.name += '.';
        offset += length;
    }
    if (question.name.empty()) question.name = ".";
    if (offset + 4 > packet.size() || question.name.size() > 1008) return std::nullopt;
    question.type = read16(packet, offset);
    question.recordClass = read16(packet, offset + 2);
    offset += 4;
    question.wire.assign(packet.begin() + 12, packet.begin() + offset);
    // An OPT record among the additional ones says how large an answer may be.
    const unsigned others = read16(packet, 6) + read16(packet, 8) + read16(packet, 10);
    if (others == 1 && offset + 11 <= packet.size() && packet[offset] == 0 && read16(packet, offset + 1) == 41) {
        question.extended = true;
        question.answerLimit = std::clamp<uint16_t>(read16(packet, offset + 3), 512, 4096);
    }
    return question;
}

// A name as dns_sd writes it, in the labels of the wire.
inline bool appendName(std::vector<uint8_t>& out, std::string_view name) {
    std::vector<uint8_t> label;
    const auto flush = [&] {
        if (label.empty() || label.size() > 63) return false;
        out.push_back(uint8_t(label.size()));
        out.insert(out.end(), label.begin(), label.end());
        label.clear();
        return true;
    };
    for (size_t index = 0; index < name.size(); ++index) {
        const char byte = name[index];
        if (byte == '.') {
            if (!flush()) return name == ".";
        } else if (byte == '\\' && index + 1 < name.size()) {
            if (index + 3 < name.size() && isdigit(name[index + 1]) && isdigit(name[index + 2]) && isdigit(name[index + 3])) {
                label.push_back(uint8_t((name[index + 1] - '0') * 100 + (name[index + 2] - '0') * 10 + (name[index + 3] - '0')));
                index += 3;
            } else {
                label.push_back(uint8_t(name[++index]));
            }
        } else {
            label.push_back(uint8_t(byte));
        }
    }
    if (!label.empty() && !flush()) return false;
    out.push_back(0);
    return true;
}

// The answer to a question: its records, as many as fit, under a response code.
inline std::vector<uint8_t> respond(const Question& question, std::span<const Record> records, uint8_t code) {
    std::vector<uint8_t> out;
    append16(out, question.id);
    append16(out, uint16_t(0x8080 | (question.recursionDesired ? 0x0100 : 0) | code));
    append16(out, 1);
    append16(out, 0);
    append16(out, 0);
    append16(out, question.extended ? 1 : 0);
    out.insert(out.end(), question.wire.begin(), question.wire.end());
    const size_t limit = question.answerLimit - (question.extended ? 11 : 0);
    uint16_t count = 0;
    bool truncated = false, answered = false;
    // Names that are already in the packet are written as a pointer to where they stand. A name
    // behind three aliases, each a long name of a content network, otherwise did not fit into 512
    // bytes; the answer went out cut short, and the asker's second try, by TCP, is not served here.
    const auto lower = [](std::vector<uint8_t> name) {
        for (auto& byte : name) byte = uint8_t(tolower(byte));
        return name;
    };
    std::vector<std::pair<std::vector<uint8_t>, uint16_t>> written;
    if (question.wire.size() > 4)
        written.emplace_back(lower({question.wire.begin(), question.wire.end() - 4}), uint16_t(12));
    for (const auto& record : records) {
        std::vector<uint8_t> owner;
        if (!appendName(owner, record.name) || record.data.size() > 0xffff) continue;
        std::vector<uint8_t> entry;
        const auto known = std::find_if(written.begin(), written.end(), [&](const auto& item) { return item.first == lower(owner); });
        if (known != written.end()) append16(entry, uint16_t(0xc000 | known->second));
        else entry = owner;
        append16(entry, record.type);
        append16(entry, record.recordClass);
        append16(entry, uint16_t(record.timeToLive >> 16));
        append16(entry, uint16_t(record.timeToLive));
        append16(entry, uint16_t(record.data.size()));
        const size_t dataAt = out.size() + entry.size();
        entry.insert(entry.end(), record.data.begin(), record.data.end());
        if (out.size() + entry.size() > limit) {
            truncated = true;
            break;
        }
        // What an alias leads to is the name of the records that follow it.
        if (record.type == 5 && dataAt < 0x4000) written.emplace_back(lower(record.data), uint16_t(dataAt));
        out.insert(out.end(), entry.begin(), entry.end());
        answered = answered || record.type == question.type;
        ++count;
    }
    out[6] = uint8_t(count >> 8);
    out[7] = uint8_t(count);
    // Cut short is only said when nothing of what was asked for is in the answer: with some of the
    // addresses in it the asker has what it needs.
    if (truncated && !answered) out[2] |= 0x02;
    if (question.extended) {
        out.push_back(0);
        append16(out, 41);
        append16(out, 4096);
        append16(out, 0);
        append16(out, 0);
        append16(out, 0);
    }
    return out;
}

struct Lookup {
    const Question* question = nullptr;
    std::vector<Record> records;
    bool done = false, failed = false;
};

inline void DNSSD_API lookupReply(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType error,
                                  const char* name, uint16_t type, uint16_t recordClass, uint16_t length,
                                  const void* data, uint32_t timeToLive, void* context) {
    auto& lookup = *static_cast<Lookup*>(context);
    if (error == kDNSServiceErr_NoError) {
        if (flags & kDNSServiceFlagsAdd) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            lookup.records.push_back({name ? name : "", type, recordClass, timeToLive, std::vector<uint8_t>(bytes, bytes + length)});
            // An alias on the way to what was asked for is followed by more; the record asked for ends it.
            const bool wanted = type == lookup.question->type || lookup.question->type == 255;
            if (wanted && !(flags & kDNSServiceFlagsMoreComing)) lookup.done = true;
        }
    } else {
        // No such name, or no record of the type under it: the answer is an empty one.
        lookup.failed = error != kDNSServiceErr_NoSuchRecord && error != kDNSServiceErr_NoSuchName;
        lookup.done = true;
    }
}

// The response to one query packet, from the system's resolver. Nothing for what is not a query.
inline std::optional<std::vector<uint8_t>> answer(std::span<const uint8_t> packet,
                                                  std::chrono::milliseconds patience = std::chrono::seconds(8)) {
    const auto question = parse(packet);
    if (!question) return std::nullopt;
    // Names under .local are asked of the local network by multicast, which is not this server's work.
    const auto endsWith = [&](std::string_view suffix) {
        const std::string_view name = question->name;
        if (name.size() < suffix.size()) return false;
        for (size_t index = 0; index < suffix.size(); ++index)
            if (tolower(name[name.size() - suffix.size() + index]) != suffix[index]) return false;
        return name.size() == suffix.size() || name[name.size() - suffix.size() - 1] == '.';
    };
    if (question->recordClass != 1 || endsWith("local.")) return respond(*question, {}, 3);
    Lookup lookup;
    lookup.question = &*question;
    DNSServiceRef service = nullptr;
    if (DNSServiceQueryRecord(&service, kDNSServiceFlagsReturnIntermediates | kDNSServiceFlagsTimeout, 0,
                              question->name.c_str(), question->type, question->recordClass, lookupReply, &lookup) != kDNSServiceErr_NoError)
        return respond(*question, {}, 2);
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!lookup.done) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd entry{DNSServiceRefSockFD(service), POLLIN, 0};
        if (::poll(&entry, 1, int(left)) <= 0) continue;
        if (DNSServiceProcessResult(service) != kDNSServiceErr_NoError) {
            lookup.failed = true;
            break;
        }
    }
    DNSServiceRefDeallocate(service);
    // Aliases without the record they lead to are still an answer; nothing at all after the wait is a failure.
    if (lookup.failed || (!lookup.done && lookup.records.empty())) return respond(*question, {}, 2);
    return respond(*question, lookup.records, 0);
}

// The port, in host order, of the server on the loopback; it starts with the first call. Zero when it
// could not start.
inline uint16_t port() {
    static const uint16_t bound = [] {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return uint16_t(0);
        sockaddr_in address{};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ||
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length)) {
            ::close(fd);
            return uint16_t(0);
        }
        std::thread([fd] {
            for (;;) {
                std::vector<uint8_t> packet(4096);
                sockaddr_in from{};
                socklen_t fromLength = sizeof(from);
                const auto size = ::recvfrom(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
                if (size < 0) {
                    if (errno == EINTR) continue;
                    return;
                }
                packet.resize(size_t(size));
                // Each query waits on the resolver by itself, so a slow name does not hold up the others.
                std::thread([fd, from, fromLength, packet = std::move(packet)] {
                    if (const auto reply = answer(packet))
                        ::sendto(fd, reply->data(), reply->size(), 0, reinterpret_cast<const sockaddr*>(&from), fromLength);
                }).detach();
            }
        }).detach();
        return ntohs(address.sin_port);
    }();
    return bound;
}

} // namespace nestvane::dnsproxy
