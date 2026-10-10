#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include "protocol.hpp"
#include "udp_socket.hpp"
#include "../simulation/authority.hpp"

namespace voxel::net {
class Server {
public:
    explicit Server(std::uint16_t port = 27015);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    void start();
    void stop();
private:
    void run();
    void receive();
    std::uint16_t m_port;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    UdpSocket m_socket;
    Authority m_authority{1};
    std::mutex m_mutex;
    InputIntent m_latest{};
    std::uint32_t m_lastClientTick = 0;
    Peer m_peer{};
    bool m_worldSent = false;
};
}
