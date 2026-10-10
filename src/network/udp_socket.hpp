#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "address.hpp"

namespace voxel::net {

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind(std::uint16_t port);
    void nonBlocking();
    bool send(std::span<const std::byte> data, const Peer& peer) const;
    std::ptrdiff_t receive(std::span<std::byte> data, Peer& peer) const;

private:
    int m_handle = -1;
};
}
