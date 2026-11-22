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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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
    if (four.empty()) return address[0] == 10 && address[8] == 0xff;
    if (four[0] >= 224) return true;
    return four[0] == guestAddress[0] && four[1] == guestAddress[1] && four[2] == guestAddress[2] && !isNameServer(address);
}

inline bool isNameServer(const Address& address) {
    const auto four = ipv4(address);
    return !four.empty() && std::equal(four.begin(), four.end(), nameServer.begin());
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

// The guest's own address with a port, as the local address of a socket of the family.
inline Address localAddress(uint32_t family, uint16_t port) {
    Address guest{};
    guest[0] = uint8_t(family);
    guest[2] = uint8_t(port >> 8);
    guest[3] = uint8_t(port);
    if (family == 2) {
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
inline int openDatagram(const Address& destination) {
    sockaddr_storage host;
    toHost(destination, host);
    return prepare(::socket(host.ss_family, SOCK_DGRAM, IPPROTO_UDP));
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
    return (entry.revents & POLLIN ? 1 : 0) | (entry.revents & POLLOUT ? 4 : 0) | (entry.revents & POLLERR ? 8 : 0) |
           (entry.revents & POLLHUP ? 16 : 0);
}

} // namespace nestvane::hostnet
