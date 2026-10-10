#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace voxel::net {

class TcpSocket {
public:
    // Frames are length-prefixed little-endian and capped at 64 MiB. The
    // blocking receive API is intended for a dedicated world-stream thread.
    TcpSocket() = default;
    explicit TcpSocket(int handle) : m_handle(handle) {}
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    void listen(std::uint16_t port, int backlog = 16);
    TcpSocket accept();
    void connect(const char* host, std::uint16_t port);
    void nonBlocking();
    void shutdown();
    bool valid() const { return m_handle >= 0; }
    bool sendFrame(std::span<const std::byte> payload) const;
    bool receiveFrame(std::vector<std::byte>& payload) const;

private:
    void close();
    int m_handle = -1;
};

} // namespace voxel::net
