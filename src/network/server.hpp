#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <memory>
#include "protocol.hpp"
#include "udp_socket.hpp"
#include "../simulation/svo.hpp"
#include "tcp_socket.hpp"
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
    struct ClientConnection {
        ClientUuid uuid{};
        Peer peer{};
        std::unique_ptr<Authority> authority;
        InputIntent latest{};
        bool jumpRequested = false;
        std::uint32_t lastClientTick = 0;
    };

    void run();
    void worldRun();
    void receive();
    std::uint16_t m_port;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;
    UdpSocket m_socket;
    TcpSocket m_worldListener;
    std::mutex m_mutex;
    std::vector<ClientConnection> m_clients;
    std::shared_ptr<SvoWorld> m_world;
    std::uint32_t m_nextPlayerId = 1;
    std::uint32_t m_tick = 0;
};
}
