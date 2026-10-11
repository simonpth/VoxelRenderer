#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace voxel::net {

// Length-prefixed TCP frames. Blocking by default so it can be driven from a
// dedicated world-stream thread; `pollReadable` lets that thread interleave
// sending and receiving without extra threads.
class TcpSocket {
  public:
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

    // Milliseconds to wait before deciding the socket is idle.
    bool pollReadable(int timeoutMs) const;

    bool sendFrame(std::span<const std::byte> payload) const;
    bool receiveFrame(std::vector<std::byte>& payload) const;

  private:
    void close();
    int m_handle = -1;
};

} // namespace voxel::net
