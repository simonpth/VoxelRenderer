#include "udp_socket.hpp"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace voxel::net {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
}
}

UdpSocket::UdpSocket() {
    m_handle = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(m_handle >= 0, "create UDP socket");
}
UdpSocket::~UdpSocket() {
    if (m_handle >= 0) ::close(m_handle);
}
void UdpSocket::bind(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    require(::bind(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "bind UDP socket");
}
void UdpSocket::nonBlocking() {
    const int flags = fcntl(m_handle, F_GETFL, 0);
    require(flags >= 0 && fcntl(m_handle, F_SETFL, flags | O_NONBLOCK) == 0,
            "set UDP socket non-blocking");
}
bool UdpSocket::send(std::span<const std::byte> data, const Peer& peer) const {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(peer.port);
    if (inet_pton(AF_INET, peer.host.c_str(), &address.sin_addr) != 1) return false;
    return ::sendto(m_handle, data.data(), data.size(), 0,
                    reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
           static_cast<ssize_t>(data.size());
}
std::ptrdiff_t UdpSocket::receive(std::span<std::byte> data, Peer& peer) const {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const auto result = ::recvfrom(m_handle, data.data(), data.size(), 0,
                                   reinterpret_cast<sockaddr*>(&address), &length);
    if (result < 0) return 0;
    char host[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    peer = {host, ntohs(address.sin_port)};
    return result;
}
}
