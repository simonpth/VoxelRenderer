#include "tcp_socket.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace voxel::net {
namespace {
constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

void writeU32(std::byte* output, std::uint32_t value) {
    output[0] = std::byte(value);
    output[1] = std::byte(value >> 8);
    output[2] = std::byte(value >> 16);
    output[3] = std::byte(value >> 24);
}

std::uint32_t readU32(const std::byte* input) {
    return std::to_integer<std::uint32_t>(input[0]) |
           (std::to_integer<std::uint32_t>(input[1]) << 8) |
           (std::to_integer<std::uint32_t>(input[2]) << 16) |
           (std::to_integer<std::uint32_t>(input[3]) << 24);
}

bool transfer(int handle, std::byte* data, std::size_t size, bool send) {
    while (size != 0) {
        const auto result = send
            ? ::send(handle, data, size, MSG_NOSIGNAL)
            : ::recv(handle, data, size, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        data += result;
        size -= static_cast<std::size_t>(result);
    }
    return true;
}
}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this == &other) return *this;
    close();
    m_handle = other.m_handle;
    other.m_handle = -1;
    return *this;
}

void TcpSocket::close() {
    if (m_handle >= 0) {
        ::shutdown(m_handle, SHUT_RDWR);
        ::close(m_handle);
        m_handle = -1;
    }
}

void TcpSocket::shutdown() {
    if (m_handle >= 0) ::shutdown(m_handle, SHUT_RDWR);
}

void TcpSocket::listen(std::uint16_t port, int backlog) {
    close();
    m_handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_handle < 0) throw std::runtime_error("create TCP socket failed");
    int reuse = 1;
    ::setsockopt(m_handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(m_handle, backlog) != 0) {
        close();
        throw std::runtime_error(std::string("listen TCP socket failed: ") +
                                 std::strerror(errno));
    }
}

TcpSocket TcpSocket::accept() {
    const int accepted = ::accept(m_handle, nullptr, nullptr);
    if (accepted < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return TcpSocket{};
    if (accepted >= 0) {
        const timeval timeout{5, 0};
        ::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    return TcpSocket(accepted);
}

void TcpSocket::connect(const char* host, std::uint16_t port) {
    close();
    m_handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_handle < 0) throw std::runtime_error("create TCP client socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        ::connect(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close();
        throw std::runtime_error(std::string("connect TCP socket failed: ") +
                                 std::strerror(errno));
    }
    const timeval timeout{5, 0};
    ::setsockopt(m_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(m_handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

void TcpSocket::nonBlocking() {
    const int flags = fcntl(m_handle, F_GETFL, 0);
    if (flags < 0 || fcntl(m_handle, F_SETFL, flags | O_NONBLOCK) != 0)
        throw std::runtime_error("set TCP socket non-blocking failed");
}

bool TcpSocket::sendFrame(std::span<const std::byte> payload) const {
    if (!valid() || payload.empty() || payload.size() > kMaxFrameBytes) return false;
    std::array<std::byte, 4> header{};
    writeU32(header.data(), static_cast<std::uint32_t>(payload.size()));
    return transfer(m_handle, header.data(), header.size(), true) &&
           transfer(m_handle, const_cast<std::byte*>(payload.data()), payload.size(), true);
}

bool TcpSocket::receiveFrame(std::vector<std::byte>& payload) const {
    std::array<std::byte, 4> header{};
    if (!valid() || !transfer(m_handle, header.data(), header.size(), false)) return false;
    const auto size = readU32(header.data());
    if (size == 0 || size > kMaxFrameBytes) return false;
    payload.resize(size);
    return transfer(m_handle, payload.data(), payload.size(), false);
}

} // namespace voxel::net
