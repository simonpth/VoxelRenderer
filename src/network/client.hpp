#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "../simulation/svo.hpp"
#include "../simulation/state.hpp"

namespace voxel::net {

// ---------------------------------------------------------------------------
// Client
//
// Keeps no voxel data on the CPU. It forwards intent over UDP, receives
// authoritative snapshots over UDP, streams bricks over TCP, hands the renderer
// a transient upload queue, and interpolates transform state for display.
// ---------------------------------------------------------------------------

class Client {
  public:
    Client(Peer server, std::string host = "127.0.0.1");
    ~Client() noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start();
    void stop();
    bool running() const { return m_running.load(); }

    // Called once per render frame with the raw local input.
    void submit(const InputIntent& intent, float frameSeconds);
    void updateRender(float frameSeconds);
    RenderState& renderState() { return m_render; }
    const RenderState& renderState() const { return m_render; }
    const AuthoritativeState& authoritativeState() const {
        return m_authoritative;
    }

  private:
    struct RegionKeyHash {
        std::size_t operator()(std::uint32_t value) const noexcept {
            return std::hash<std::uint32_t>{}(value);
        }
    };
    // Bookkeeping only: which brick revision the client believes it holds and
    // which root-layer neighbours it still wants. No voxel data is stored.
    struct Resident {
        std::uint32_t revision = 0;
    };
    void receiveLoop();
    void worldReceiveLoop();
    void acceptSnapshot(AuthoritativeState snapshot);
    void enqueueBrick(BrickSnapshot snapshot);
    void updateInterest();

    Peer m_server;
    std::string m_host;
    voxel::WorldSpec m_spec{};
    // Copy owned by the world stream thread only.
    voxel::WorldSpec m_streamSpec{};
    bool m_hasSpec = false;
    std::vector<Material> m_materials;

    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;

    UdpSocket m_socket;
    TcpSocket m_worldSocket;

    mutable std::mutex m_mutex;
    std::deque<AuthoritativeState> m_snapshots;
    std::deque<BrickSnapshot> m_worldQueue;
    std::vector<WorldAck> m_pendingAcks;
    std::unordered_map<std::uint32_t, Resident, RegionKeyHash> m_resident;
    std::int32_t m_interestX = 0;
    std::int32_t m_interestY = 0;
    std::int32_t m_interestZ = 0;
    bool m_hasInterest = false;

    AuthoritativeState m_previous{};
    AuthoritativeState m_next{};
    bool m_hasSnapshots = false;
    AuthoritativeState m_authoritative{};
    RenderState m_render{};

    float m_clock = 0.0f;
    float m_localYaw = 0.0f;
    float m_localPitch = 0.0f;
    glm::vec2 m_accumulatedLook{0.0f};
    std::uint32_t m_latestClientTick = 0;
    bool m_jumpRequested = false;
    ClientUuid m_uuid{};
    bool m_registered = false;
    std::uint32_t m_playerId = 0;
    float m_registrationRetry = 0.0f;
    float m_inputAccumulator = voxel::kTickSeconds;
    std::uint8_t m_interestRadius = 6;
};

} // namespace voxel::net
