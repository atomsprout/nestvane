#pragma once

// The guest's way out to the network: its TCP and UDP sockets to addresses that are not its own are
// carried by sockets of the host. The guest sees the network of the Android emulator's user mode:
// it is 10.0.2.15, its gateway 10.0.2.2, and its name server 10.0.2.3, which is answered from the
// system's resolver.

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

#include "DnsProxy.hpp"

namespace nestvane::hostnet {

// A Linux sockaddr_in or sockaddr_in6 as the guest passes it: family, port in network order, then the
// address (for IPv6 after four bytes of flow information).
using Address = std::array<uint8_t, 28>;
inline constexpr std::array<uint8_t, 4> guestAddress{10, 0, 2, 15}, gatewayAddress{10, 0, 2, 2}, nameServer{10, 0, 2, 3};
inline constexpr std::array<uint8_t, 6> hardwareAddress{0x02, 0x4e, 0x56, 0x00, 0x00, 0x01};
inline constexpr unsigned prefixLength = 24;

// The IPv4 address in a guest address, also from an IPv4-mapped IPv6 address, or nothing.
inline std::span<const uint8_t> ipv4(const Address& address) {
    if (address[0] == 2) return std::span(address).subspan(4, 4);
    if (address[0] != 10) return {};
    for (size_t index = 8; index < 18; ++index)
        if (address[index]) return {};
    if (address[18] != 0xff || address[19] != 0xff) return {};
    return std::span(address).subspan(20, 4);
}

// Whether an address is out on the network: neither unspecified nor the guest's loopback.
inline bool external(const Address& address) {
    if (const auto four = ipv4(address); !four.empty())
        return four[0] != 127 && (four[0] | four[1] | four[2] | four[3]);
    if (address[0] != 10) return false;
    uint8_t any = 0;
    for (size_t index = 8; index < 23; ++index) any |= address[index];
    return any || address[23] > 1;
}

// Linux error numbers for the host's.
inline int linuxError(int error) {
    switch (error) {
        case EAGAIN: return 11;
        case EINPROGRESS: return 115;
        case EALREADY: return 114;
        case ECONNREFUSED: return 111;
        case ETIMEDOUT: return 110;
        case ENETUNREACH: return 101;
        case ENETDOWN: return 100;
        case EHOSTUNREACH: return 113;
        case ECONNRESET: return 104;
        case ECONNABORTED: return 103;
        case ENOTCONN: return 107;
        case EISCONN: return 106;
        case EADDRINUSE: return 98;
        case EADDRNOTAVAIL: return 99;
        case EMSGSIZE: return 90;
        case ENOBUFS: return 105;
        case EPIPE: return 32;
        case EINTR: return 4;
        case EACCES: return 13;
        case EPERM: return 1;
        case EINVAL: return 22;
        case EAFNOSUPPORT: return 97;
        default: return 5;
    }
}

// What stands behind the guest's 10.0.2.3: the server on the host's loopback that answers from the
// system's resolver (DnsProxy.hpp). The network's own name server is usually the router, and an app
// that sends to it is asked by iOS for access to the local network; this way none is needed.
inline sockaddr_in hostNameServer(uint16_t guestPort) {
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Only queries by UDP are answered. Port 1 for everything else: nothing listens there.
    address.sin_port = htons(guestPort == 53 && dnsproxy::port() ? dnsproxy::port() : 1);
    return address;
}

// Destinations the guest's packets are not carried to: multicast and broadcast, which iOS counts as
// reaching the local network and which bring no answers back through this bridge, and the guest's
// own made-up subnet apart from its name server.
inline bool isNameServer(const Address& address);
inline bool unreachable(const Address& address) {
    const auto four = ipv4(address);
    // IPv6: multicast, and the link's own addresses (fe80::/10), which need an interface of the host's.
    if (four.empty()) return address[0] == 10 && (address[8] == 0xff || (address[8] == 0xfe && (address[9] & 0xc0) == 0x80));
    if (four[0] >= 224) return true;
    return four[0] == guestAddress[0] && four[1] == guestAddress[1] && four[2] == guestAddress[2] && !isNameServer(address);
}

inline bool isNameServer(const Address& address) {
    const auto four = ipv4(address);
    return !four.empty() && std::equal(four.begin(), four.end(), nameServer.begin());
}

// A network with no IPv4 of its own, as many cellular networks are, carries IPv4 through a gateway:
// an IPv4 address is reached at an IPv6 address made of the network's prefix and the four bytes
// (NAT64). The guest knows only IPv4, so its destinations are rewritten here. The system says
// whether the network is such a one when it is asked to resolve an IPv4 address written out: it
// answers with the IPv6 address to use, and with the IPv4 address itself where IPv4 works.
// The answer is asked for again when the app reports a change of network, and every few seconds.
inline std::atomic_bool& networkChangedFlag() {
    static std::atomic_bool changed{true};
    return changed;
}
inline void networkChanged() { networkChangedFlag().store(true, std::memory_order_relaxed); }

inline bool nat64Prefix(std::array<uint8_t, 12>& prefix) {
    static std::mutex mutex;
    static std::array<uint8_t, 12> known{};
    static bool present = false;
    static std::chrono::steady_clock::time_point asked{};
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (networkChangedFlag().exchange(false, std::memory_order_relaxed) || now - asked > std::chrono::seconds(5)) {
        asked = now;
        static constexpr uint8_t probe[4] = {192, 0, 2, 1};
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
#ifdef AI_DEFAULT
        hints.ai_flags = AI_DEFAULT;
#else
        hints.ai_flags = AI_ADDRCONFIG;
#endif
        addrinfo* answers = nullptr;
        bool four = false, six = false;
        if (getaddrinfo("192.0.2.1", nullptr, &hints, &answers) == 0) {
            for (const addrinfo* answer = answers; answer; answer = answer->ai_next) {
                if (answer->ai_family == AF_INET) four = true;
                if (answer->ai_family != AF_INET6 || six) continue;
                const auto* bytes = reinterpret_cast<const sockaddr_in6*>(answer->ai_addr)->sin6_addr.s6_addr;
                // Only prefixes of 96 bits, which put the IPv4 address in the last four bytes.
                if (std::memcmp(bytes + 12, probe, 4) != 0) continue;
                std::memcpy(known.data(), bytes, 12);
                six = true;
            }
            freeaddrinfo(answers);
        }
        present = six && !four;
    }
    if (present) prefix = known;
    return present;
}

// The host address for a guest destination.
inline socklen_t toHost(const Address& guest, sockaddr_storage& host) {
    host = {};
    if (isNameServer(guest)) {
        const auto server = hostNameServer(uint16_t(guest[2] << 8 | guest[3]));
        std::memcpy(&host, &server, sizeof(server));
        return sizeof(server);
    }
    if (const auto four = ipv4(guest); !four.empty()) {
        if (std::array<uint8_t, 12> prefix; nat64Prefix(prefix)) {
            auto& address = reinterpret_cast<sockaddr_in6&>(host);
            address.sin6_len = sizeof(address);
            address.sin6_family = AF_INET6;
            std::memcpy(&address.sin6_port, guest.data() + 2, 2);
            std::memcpy(address.sin6_addr.s6_addr, prefix.data(), 12);
            std::memcpy(address.sin6_addr.s6_addr + 12, four.data(), 4);
            return sizeof(address);
        }
        auto& address = reinterpret_cast<sockaddr_in&>(host);
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        std::memcpy(&address.sin_port, guest.data() + 2, 2);
        std::memcpy(&address.sin_addr, four.data(), 4);
        return sizeof(address);
    }
    auto& address = reinterpret_cast<sockaddr_in6&>(host);
    address.sin6_len = sizeof(address);
    address.sin6_family = AF_INET6;
    std::memcpy(&address.sin6_port, guest.data() + 2, 2);
    std::memcpy(&address.sin6_addr, guest.data() + 8, 16);
    return sizeof(address);
}

// The guest address, of the guest socket's family, for a host address. What comes from the host's
// name server comes from 10.0.2.3 for the guest.
inline Address toGuest(const sockaddr_storage& host, uint32_t family) {
    Address guest{};
    guest[0] = uint8_t(family);
    std::array<uint8_t, 4> four{};
    bool hasFour = false;
    const bool fromServer = host.ss_family == AF_INET &&
        reinterpret_cast<const sockaddr_in&>(host).sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    std::memcpy(guest.data() + 2, &reinterpret_cast<const sockaddr_in&>(host).sin_port, 2);
    if (fromServer) {
        guest[2] = 0;
        guest[3] = 53;
        four = nameServer;
        hasFour = true;
    } else if (host.ss_family == AF_INET) {
        std::memcpy(four.data(), &reinterpret_cast<const sockaddr_in&>(host).sin_addr, 4);
        hasFour = true;
    } else if (std::array<uint8_t, 12> prefix; host.ss_family == AF_INET6 && nat64Prefix(prefix) &&
               std::memcmp(reinterpret_cast<const sockaddr_in6&>(host).sin6_addr.s6_addr, prefix.data(), 12) == 0) {
        // What came through the gateway came from the IPv4 address in its last four bytes.
        std::memcpy(four.data(), reinterpret_cast<const sockaddr_in6&>(host).sin6_addr.s6_addr + 12, 4);
        hasFour = true;
    }
    if (family == 2) {
        if (hasFour) std::memcpy(guest.data() + 4, four.data(), 4);
    } else if (hasFour) {
        guest[18] = guest[19] = 0xff;
        std::memcpy(guest.data() + 20, four.data(), 4);
    } else {
        std::memcpy(guest.data() + 8, &reinterpret_cast<const sockaddr_in6&>(host).sin6_addr, 16);
    }
    return guest;
}

// The guest's IPv6 address, for its sockets to IPv6 destinations: a unique local address (fd00::/8)
// made like its IPv4 one. Android ranks a unique local source below IPv4 for a global destination, so
// that where a name has both, IPv4 is tried first, as it always was here.
inline constexpr std::array<uint8_t, 16> guestAddress6{0xfd, 0x00, 0, 0x10, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0, 0x15};

// Whether a guest address is an IPv6 one, not IPv4 written as IPv6.
inline bool isIpv6(const Address& address) { return address[0] == 10 && ipv4(address).empty(); }

// Whether the host has a route to an IPv6 destination: connecting a datagram socket sends nothing.
inline bool hostRoutes(const Address& destination) {
    sockaddr_in6 host{};
    host.sin6_len = sizeof(host);
    host.sin6_family = AF_INET6;
    host.sin6_port = htons(9);
    std::memcpy(&host.sin6_addr, destination.data() + 8, 16);
    const int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    const bool routed = ::connect(fd, reinterpret_cast<const sockaddr*>(&host), sizeof(host)) == 0;
    ::close(fd);
    return routed;
}

// The guest's own address with a port, as the local address of a socket of the family. For an IPv6
// destination, the guest's IPv6 address.
inline Address localAddress(uint32_t family, uint16_t port, bool ipv6 = false) {
    Address guest{};
    guest[0] = uint8_t(family);
    guest[2] = uint8_t(port >> 8);
    guest[3] = uint8_t(port);
    if (family == 10 && ipv6) {
        std::memcpy(guest.data() + 8, guestAddress6.data(), 16);
    } else if (family == 2) {
        std::memcpy(guest.data() + 4, guestAddress.data(), 4);
    } else {
        guest[18] = guest[19] = 0xff;
        std::memcpy(guest.data() + 20, guestAddress.data(), 4);
    }
    return guest;
}

inline int prepare(int fd) {
    if (fd < 0) return -linuxError(errno);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    return fd;
}

// The TCP connections that are open, so that all of them can be ended when the phone moves to another
// network. A connection made over Wi-Fi does not move to cellular: left alone it stays silent, and
// the app that holds it waits out its timeout on every request before it tries a new one. Android
// ends such connections itself when a network goes away; here the guest has one made-up network
// that never does, so the host ends them: the guest reads the end of the stream and connects again.
inline std::mutex& streamsMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::set<int>& streams() {
    static std::set<int> open;
    return open;
}
inline size_t endStreams() {
    std::lock_guard lock(streamsMutex());
    for (const int fd : streams()) ::shutdown(fd, SHUT_RDWR);
    return streams().size();
}
// Closes a host socket of either kind.
inline void closeSocket(int fd) {
    std::lock_guard lock(streamsMutex());
    streams().erase(fd);
    ::close(fd);
}

// Starts a TCP connection to a guest destination. A host socket that never blocks, or a negative
// Linux error number.
inline int openStream(const Address& destination) {
    sockaddr_storage host;
    const socklen_t length = toHost(destination, host);
    const int fd = prepare(::socket(host.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return fd;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&host), length) && errno != EINPROGRESS) {
        const int error = linuxError(errno);
        ::close(fd);
        return -error;
    }
    std::lock_guard lock(streamsMutex());
    streams().insert(fd);
    return fd;
}

// How a connection that was started stands: 0 when it is made, -115 while it is being made, or
// another negative Linux error number.
inline int connectState(int fd) {
    pollfd entry{fd, POLLOUT, 0};
    if (::poll(&entry, 1, 0) <= 0) return -115;
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length)) error = errno;
    return error ? -linuxError(error) : 0;
}

// A UDP socket of the family that reaches a guest destination.
inline int openDatagram(const Address& destination, bool icmp = false) {
    sockaddr_storage host;
    toHost(destination, host);
    // An echo socket, which ping uses, is a datagram socket of ICMP: an app may have one without privileges.
    if (icmp) return prepare(::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP));
    return prepare(::socket(host.ss_family, SOCK_DGRAM, IPPROTO_UDP));
}

// The Internet checksum of an ICMP message, written into it. Linux works it out for an echo socket;
// this system leaves it to the sender.
inline void sealIcmp(std::span<uint8_t> message) {
    if (message.size() < 8) return;
    message[2] = message[3] = 0;
    uint32_t sum = 0;
    for (size_t at = 0; at + 1 < message.size(); at += 2) sum += uint32_t(message[at]) << 8 | message[at + 1];
    if (message.size() & 1) sum += uint32_t(message.back()) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    const uint16_t checksum = uint16_t(~sum);
    message[2] = uint8_t(checksum >> 8);
    message[3] = uint8_t(checksum);
}

// What arrived on an echo socket, as Linux hands it on: the ICMP message alone. This system puts the
// IP header in front of it.
inline void stripIpHeader(std::vector<uint8_t>& packet) {
    if (packet.size() < 20 || packet[0] >> 4 != 4) return;
    const size_t header = size_t(packet[0] & 15) * 4;
    if (header < 20 || packet.size() < header + 8) return;
    packet.erase(packet.begin(), packet.begin() + long(header));
}

inline int connectDatagram(int fd, const Address& destination) {
    sockaddr_storage host;
    const socklen_t length = toHost(destination, host);
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&host), length) ? -linuxError(errno) : 0;
}

inline int64_t send(int fd, std::span<const uint8_t> bytes) {
    const auto sent = ::send(fd, bytes.data(), bytes.size(), 0);
    return sent < 0 ? -int64_t(linuxError(errno)) : int64_t(sent);
}

inline int64_t sendTo(int fd, std::span<const uint8_t> bytes, const Address& destination) {
    sockaddr_storage host;
    const socklen_t length = toHost(destination, host);
    const auto sent = ::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&host), length);
    return sent < 0 ? -int64_t(linuxError(errno)) : int64_t(sent);
}

inline int64_t receive(int fd, std::span<uint8_t> bytes, bool peek = false) {
    const auto received = ::recv(fd, bytes.data(), bytes.size(), peek ? MSG_PEEK : 0);
    return received < 0 ? -int64_t(linuxError(errno)) : int64_t(received);
}

inline int64_t receiveFrom(int fd, std::span<uint8_t> bytes, uint32_t family, Address& source, bool peek = false) {
    sockaddr_storage host{};
    socklen_t length = sizeof(host);
    const auto received = ::recvfrom(fd, bytes.data(), bytes.size(), peek ? MSG_PEEK : 0, reinterpret_cast<sockaddr*>(&host), &length);
    if (received < 0) return -int64_t(linuxError(errno));
    source = toGuest(host, family);
    return int64_t(received);
}

// What poll says of the socket now, in Linux's bits: 1 readable, 4 writable, 8 error, 16 hung up.
inline int events(int fd) {
    pollfd entry{fd, POLLIN | POLLOUT, 0};
    if (::poll(&entry, 1, 0) <= 0) return 0;
    int result = (entry.revents & POLLIN ? 1 : 0) | (entry.revents & POLLOUT ? 4 : 0) | (entry.revents & POLLERR ? 8 : 0);
    if (entry.revents & POLLHUP) {
        // This system says "hung up" as soon as one direction is shut, also when it was this side
        // that finished sending and the answer is still to come. Linux says it when nothing more
        // can arrive. So look: data or the end of the stream is something to read; nothing yet is
        // no event at all, and a program that sent its request and waits for the reply goes on waiting.
        char byte;
        const auto waiting = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (waiting > 0) result |= 1;
        else if (waiting == 0) result |= 1 | 16;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) result |= 16;
    }
    return result;
}

} // namespace nestvane::hostnet
