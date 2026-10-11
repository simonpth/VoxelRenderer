#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "../simulation/authority.hpp"
#include "../simulation/svo.hpp"

namespace voxel::net {

// ---------------------------------------------------------------------------
// Server
//
// Owns the world, the tick timeline, and every client authority. It never
// trusts a client transform: the only UDP payload it accepts is input intent,
// and the only TCP payloads it accepts are interest and acknowledgement.
// ---------------------------------------------------------------------------

class Server {
  public:
    Server(std::uint16_t port, voxel::WorldSpec spec, std::uint64_t seed,
           std::string host = "0.0.0.0");
    ~Server() noexcept;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();
    bool running() const { return m_running.load(); }

  private:
    // Per world-stream connection state.
    struct StreamClient {
        ClientUuid uuid{};
        std::uint32_t playerId = 0;
        std::int32_t centerX = 0;
        std::int32_t centerY = 0;
        std::int32_t centerZ = 0;
        std::uint8_t radius = 1;
        bool hasInterest = false;
        mutable std::mutex mutex;
        // rootIndex -> revision already sent or acknowledged.
        std::unordered_map<std::uint32_t, std::uint32_t> acknowledged;
        std::unordered_map<std::uint32_t, std::uint32_t> sent;
    };

    struct ClientConnection {
        ClientUuid uuid{};
        Peer peer{};
        std::unique_ptr<voxel::Authority> authority;
        voxel::InputIntent latest{};
        bool jumpRequested = false;
        std::uint32_t lastClientTick = 0;
    };

    void run();                              // UDP receive + fixed tick
    void worldRun();                         // TCP listener
    void worldConnection(TcpSocket connection);
    void receive();

    ClientConnection* findClient(const ClientUuid& uuid);

    std::uint16_t m_port;
    std::string m_host;
    voxel::WorldSpec m_spec;
    std::unique_ptr<voxel::World> m_world;
    std::mutex m_worldMutex;

    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;
    std::vector<std::thread> m_streamThreads;
    std::mutex m_streamMutex;

    UdpSocket m_socket;
    TcpSocket m_worldListener;
    std::mutex m_mutex;
    std::vector<ClientConnection> m_clients;
    std::uint32_t m_nextPlayerId = 1;
    std::uint32_t m_tick = 0;
};

} // namespace voxel::net
