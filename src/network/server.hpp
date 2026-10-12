#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "../simulation/authority.hpp"
#include "../simulation/lod_rings.hpp"
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

    // Ceiling on world-stream bandwidth, in bytes per second, summed over all
    // connected clients. The whole world is streamed, so this is what turns
    // "send everything" into "send it steadily until there is nothing left".
    // Zero means unmetered.
    void setBandwidth(std::uint64_t bytesPerSecond) {
        m_bytesPerSecond = bytesPerSecond;
    }
    // Bytes per second actually being spent on the world stream, averaged over
    // a short window. Reported so the pacing is observable rather than a guess.
    std::uint64_t streamedBytesPerSecond() const {
        return m_streamedBytes.load(std::memory_order_relaxed);
    }

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
        // rootIndex -> revision already acknowledged, and when the matching frame
        // went out.
        //
        // These records are NOT scoped to the interest window any more, which is
        // what allows the arena to be larger than the window. An entry means
        // "this client currently holds it", so it is removed in exactly one
        // way: the client retracts it (WorldForget) because its bounded arena
        // threw the brick away. A brick that simply leaves the window is *not*
        // forgotten, because the client is keeping it.
        //
        // The old design purged anything outside the window every pass. That
        // was correct only while the client evicted on window exit, and it is
        // exactly wrong now: it makes every brick outside the window look
        // undelivered, so the background fill resends the world forever and the
        // client evicts what it just received to make room.
        struct Delivery {
            std::uint32_t revision = 0;
            // Monotonic milliseconds. Used only for `sent`, so a frame that
            // never arrives at the client can be retried instead of being
            // treated as delivered forever.
            std::uint64_t at = 0;
        };
        std::unordered_map<std::uint32_t, Delivery> acknowledged;
        std::unordered_map<std::uint32_t, Delivery> sent;
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
    void worldConnection(TcpSocket connection,
                        std::shared_ptr<std::atomic_bool> finished);
    void receive();

    ClientConnection* findClient(const ClientUuid& uuid);

    std::uint16_t m_port;
    std::string m_host;
    voxel::WorldSpec m_spec;
    std::unique_ptr<voxel::World> m_world;
    std::mutex m_worldMutex;

    // Bandwidth ceiling for world streaming, and the rolling measurement of what
    // is actually going out. Read without the world lock: it is a gauge, not a
    // control input.
    std::uint64_t m_bytesPerSecond = 0;
    std::atomic<std::uint64_t> m_streamedBytes{0};

    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;
    // Thread plus the flag it sets when its connection is done, so the
    // listener can reap finished streams without joining live ones.
    using StreamThread =
        std::pair<std::thread, std::shared_ptr<std::atomic_bool>>;
    std::vector<StreamThread> m_streamThreads;
    std::mutex m_streamMutex;

    UdpSocket m_socket;
    TcpSocket m_worldListener;
    std::mutex m_mutex;
    std::vector<ClientConnection> m_clients;
    std::uint32_t m_nextPlayerId = 1;
    std::uint32_t m_tick = 0;
};

} // namespace voxel::net
